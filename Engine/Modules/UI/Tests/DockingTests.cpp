#include <gtest/gtest.h>

#include "UI/Registration/ElementRegistration.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/Mount.h"
#include "UI/Layout/DockLayoutController.h"
#include "UI/Layout/Docking.h"

using namespace GameEngine;

TEST(Docking, RebuildFromModelBuildsTree) {
    // Ensure built-in controls are registered so the element factory can create nodes
    UIRegistration::RegisterBuiltInControls();

    DockingManager dm;
    // Register a few placeholder panels (nullptr content is acceptable for structure build)
    dm.RegisterPanel("Assets", nullptr);
    dm.RegisterPanel("Hierarchy", nullptr);
    dm.RegisterPanel("Inspector", nullptr);

    auto root = DockNode::MakeSplit(DockPosition::Left, 0.5f);
    root->First()->AddTab("Assets");
    root->Second()->SetSplit(DockPosition::Left, 0.5f, DockNode::MakeLeaf(), DockNode::MakeLeaf());
    root->Second()->First()->AddTab("Hierarchy");
    root->Second()->Second()->AddTab("Inspector");
    dm.SetRoot(std::move(root));

    DockspaceElement dock;
    dock.BindModel(&dm);
    dock.RebuildFromModel();

    // Expect a split/leaf structure to have been generated
    ASSERT_GE(dock.GetChildren().size(), 1u);
}



TEST(Docking, VerticalSplitAddsColumnClasses) {
    UIRegistration::RegisterBuiltInControls();

    DockingManager dm;
    auto root = std::make_unique<DockNode>();
    auto a = DockNode::MakeLeaf(); a->AddTab("A");
    auto b = DockNode::MakeLeaf(); b->AddTab("B");
    root->SetSplit(DockPosition::Bottom, 0.5f, std::move(a), std::move(b));
    dm.SetRoot(std::move(root));

    DockspaceElement dock;
    dock.BindModel(&dm);
    dock.RebuildFromModel();

    // Find the first split container and verify it has both 'column' and 'col'
    std::function<UIElement*(UIElement*)> findSplit = [&](UIElement* n) -> UIElement* {
        if (!n) return nullptr;
        if (n->HasClass("dock-split")) return n;
        for (const auto& ch : n->GetChildren()) {
            if (auto* r = findSplit(ch.get())) return r;
        }
        return nullptr;
    };

    UIElement* split = findSplit(&dock);
    ASSERT_NE(split, nullptr);
    EXPECT_TRUE(split->HasClass("column"));
    EXPECT_TRUE(split->HasClass("col"));

    // Verify the splitter handle also has 'col'
    UIElement* handle = nullptr;
    if (split) {
        const auto& ch = split->GetChildren();
        ASSERT_GE(ch.size(), 2u);
        // child[1] is the Splitter according to build order
        handle = ch.size() >= 2 ? ch[1].get() : nullptr;
    }
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(handle->HasClass("splitter"));
    EXPECT_TRUE(handle->HasClass("col"));
}

TEST(Docking, ActivationPatchSwapsTabWithoutFullRebuild) {
    UIRegistration::RegisterBuiltInControls();

    // Two tabs in a single leaf: A (active), B (inactive)
    UIElement panelA("panel-a");
    UIElement panelB("panel-b");

    DockingManager dm;
    dm.RegisterPanel("A", &panelA);
    dm.RegisterPanel("B", &panelB);

    auto leaf = DockNode::MakeLeaf();
    leaf->AddTab("A");
    leaf->AddTab("B");
    dm.SetRoot(std::move(leaf));

    DockspaceElement dock;
    dock.BindModel(&dm);
    dock.RebuildFromModel();

    const auto& instr = dock.GetInstrumentation();
    EXPECT_EQ(instr.fullRebuilds, 1u);
    EXPECT_EQ(instr.activationPatches, 0u);

    // Activate tab B via the localized patch path
    EXPECT_TRUE(dock.RequestActivationPatch("B"));
    EXPECT_EQ(instr.activationPatches, 1u);
    EXPECT_EQ(instr.fullRebuilds, 1u); // no additional full rebuild

    // Verify the model's active tab changed
    EXPECT_EQ(dm.GetRoot()->GetActivePanelId(), "B");

    // Verify the mount target was swapped
    UIElement* mountEl = dock.FindById("mount:B");
    ASSERT_NE(mountEl, nullptr);
    auto* mount = dynamic_cast<Mount*>(mountEl);
    ASSERT_NE(mount, nullptr);
    EXPECT_EQ(mount->GetTarget(), &panelB);

    // Verify tab active classes were updated
    UIElement* tabA = dock.FindById("tab:A");
    UIElement* tabB = dock.FindById("tab:B");
    ASSERT_NE(tabA, nullptr);
    ASSERT_NE(tabB, nullptr);
    EXPECT_FALSE(tabA->HasClass("active"));
    EXPECT_TRUE(tabB->HasClass("active"));
}

TEST(Docking, ActivationPatchFallsBackForInvalidPanel) {
    UIRegistration::RegisterBuiltInControls();

    UIElement panelA("panel-a");
    DockingManager dm;
    dm.RegisterPanel("A", &panelA);

    auto leaf = DockNode::MakeLeaf();
    leaf->AddTab("A");
    dm.SetRoot(std::move(leaf));

    DockspaceElement dock;
    dock.BindModel(&dm);
    dock.RebuildFromModel();

    // Request activation for a panel not in the model
    EXPECT_FALSE(dock.RequestActivationPatch("NonExistent"));

    const auto& instr = dock.GetInstrumentation();
    EXPECT_EQ(instr.activationPatches, 0u);
    // Model didn't find the panel, so ActivateTab returns false and no fallback either
    EXPECT_EQ(instr.patchFallbacks, 0u);
}

// Non-docking Mount regression: SetTarget still works correctly for standalone Mounts
TEST(Mount, SetTargetFiresLifecycleCallbacks) {
    struct TestElement : UIElement
    {
        int visibleCount = 0;
        int hiddenCount = 0;
        void OnMountVisibilityChanged(bool isVisible) override
        {
            if (isVisible) ++visibleCount;
            else ++hiddenCount;
        }
    };

    TestElement a;
    TestElement b;

    Mount mount;

    // First target: a
    mount.SetTarget(&a);
    EXPECT_EQ(a.visibleCount, 1);
    EXPECT_EQ(a.hiddenCount, 0);

    // Swap target: a detaches, b attaches
    mount.SetTarget(&b);
    EXPECT_EQ(a.visibleCount, 1);
    EXPECT_EQ(a.hiddenCount, 1);
    EXPECT_EQ(b.visibleCount, 1);
    EXPECT_EQ(b.hiddenCount, 0);

    // Set same target: no callbacks
    mount.SetTarget(&b);
    EXPECT_EQ(b.visibleCount, 1);
    EXPECT_EQ(b.hiddenCount, 0);

    // Set nullptr: b detaches
    mount.SetTarget(nullptr);
    EXPECT_EQ(b.visibleCount, 1);
    EXPECT_EQ(b.hiddenCount, 1);
}

// SwapTargetForActivation must set ChildrenDirty on the Mount so that
// BuildYogaRecursive's InsertChildrenSortedByOrder actually re-attaches
// the new target's Yoga node under the Mount. The original version of
// this test asserted the opposite (ChildrenDirty NOT set) from a time
// before commit 1d96bdd3 added the "skip InsertChildrenSortedByOrder
// when nothing structural changed" optimization — once that skip landed,
// the prior behaviour left the Mount's Yoga child list pointing at the
// OLD target after a tab switch, so the new panel had no Yoga parent
// (zero layout rect, invisible to hit-test). Users saw "can't interact
// with anything except the currently-activated tab's scroll view."
//
// The optimization the "AvoidsChildrenDirty" name referred to — avoiding
// a full dockspace rebuild via UIManagerNotifyTreeStructureChanged — is
// still preserved: SwapTargetForActivation does not call that helper,
// only SetTarget does.
TEST(Mount, SwapTargetForActivationMarksChildrenDirtyForYogaReattachment) {
    struct TestElement : UIElement
    {
        int visibleCount = 0;
        int hiddenCount = 0;
        void OnMountVisibilityChanged(bool isVisible) override
        {
            if (isVisible) ++visibleCount;
            else ++hiddenCount;
        }
    };

    TestElement a;
    TestElement b;

    Mount mount;
    mount.SetTarget(&a);
    mount.ClearDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);

    mount.SwapTargetForActivation(&b);
    EXPECT_TRUE(mount.IsDirty(UIElement::ChildrenDirty));
    EXPECT_TRUE(mount.IsDirty(UIElement::LayoutDirty));
    EXPECT_TRUE(mount.IsDirty(UIElement::VisualDirty));

    // Lifecycle callbacks should still fire
    EXPECT_EQ(a.hiddenCount, 1);
    EXPECT_EQ(b.visibleCount, 1);
}

TEST(Mount, DestructorFiresDetachCallback) {
    struct TestElement : UIElement
    {
        int hiddenCount = 0;
        void OnMountVisibilityChanged(bool isVisible) override
        {
            if (!isVisible) ++hiddenCount;
        }
    };

    TestElement a;
    {
        Mount mount;
        mount.SetTarget(&a);
    } // mount destroyed here

    EXPECT_EQ(a.hiddenCount, 1); // detach from SetTarget swap + detach from destructor
}

TEST(Docking, DockToRootBottomPlacesNewPanelAtBottom) {
    DockingManager dm;
    auto leaf = DockNode::MakeLeaf();
    leaf->AddTab("A");
    dm.SetRoot(std::move(leaf));

    ASSERT_TRUE(dm.DockToRoot(DockPosition::Bottom, "B"));
    const DockNode* root = dm.GetRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_TRUE(root->IsSplit());
    EXPECT_EQ(root->GetSplitDirection(), DockPosition::Bottom);
    const DockNode* first = root->First();
    const DockNode* second = root->Second();
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(first->IsLeaf() && second->IsLeaf());
    ASSERT_FALSE(first->GetTabs().empty());
    ASSERT_FALSE(second->GetTabs().empty());
    EXPECT_EQ(first->GetTabs()[0], "A");
    EXPECT_EQ(second->GetTabs()[0], "B");
}

TEST(Docking, DockMoveRemovalRestoresPrimarySourceTab) {
    DockingManager dm;
    auto leaf = DockNode::MakeLeaf();
    leaf->AddTab("Assets");
    leaf->AddTab("Log");
    leaf->AddTab("ScriptErrors");
    leaf->AddTab("Monitors");
    leaf->AddTab("Animation");
    ASSERT_TRUE(leaf->ActivateTab("Animation"));
    dm.SetRoot(std::move(leaf));

    ASSERT_TRUE(dm.RemoveTabForDockMove("Animation"));

    const DockNode* root = dm.GetRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_TRUE(root->IsLeaf());
    EXPECT_EQ(root->GetActivePanelId(), "Assets");
}

TEST(Docking, InactiveRemovalPreservesCurrentActiveTab) {
    DockingManager dm;
    auto leaf = DockNode::MakeLeaf();
    leaf->AddTab("Assets");
    leaf->AddTab("Log");
    leaf->AddTab("Monitors");
    ASSERT_TRUE(leaf->ActivateTab("Monitors"));
    dm.SetRoot(std::move(leaf));

    ASSERT_TRUE(dm.RemoveTab("Assets"));

    const DockNode* root = dm.GetRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_TRUE(root->IsLeaf());
    EXPECT_EQ(root->GetActivePanelId(), "Monitors");
}

TEST(Docking, ReopenClosedTabRestoresItsPreviousTabStack) {
    DockingManager dm;
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.4f);
    root->First()->AddTab("Hierarchy");
    root->First()->AddTab("SecondaryPanel");
    root->Second()->AddTab("SceneView");
    dm.SetRoot(std::move(root));

    ASSERT_TRUE(dm.RemoveTab("SecondaryPanel"));
    ASSERT_TRUE(dm.RestoreLastClosedTab("SecondaryPanel"));

    std::string path;
    const DockNode* leaf = dm.FindLeafContaining("SecondaryPanel", path);
    ASSERT_NE(leaf, nullptr);
    ASSERT_EQ(leaf->GetTabs().size(), 2u);
    EXPECT_EQ(leaf->GetTabs()[0], "Hierarchy");
    EXPECT_EQ(leaf->GetTabs()[1], "SecondaryPanel");
    EXPECT_EQ(leaf->GetActivePanelId(), "SecondaryPanel");
}

TEST(Docking, ReopenOnlyClosedTabReconstructsCollapsedSplit) {
    DockingManager dm;
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.3f);
    root->First()->AddTab("Inspector");
    root->Second()->AddTab("SceneView");
    root->Second()->AddTab("GameView");
    dm.SetRoot(std::move(root));

    ASSERT_TRUE(dm.RemoveTab("Inspector"));
    ASSERT_TRUE(dm.GetRoot()->IsLeaf());
    ASSERT_TRUE(dm.RestoreLastClosedTab("Inspector"));

    const DockNode* restored = dm.GetRoot();
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->IsSplit());
    ASSERT_TRUE(restored->First()->IsLeaf());
    ASSERT_TRUE(restored->Second()->IsLeaf());
    EXPECT_EQ(restored->First()->GetActivePanelId(), "Inspector");
    EXPECT_EQ(restored->Second()->GetTabs().size(), 2u);
    EXPECT_FLOAT_EQ(restored->GetSplitRatio(), 0.3f);
}

TEST(Docking, ReplacingLayoutClearsSessionClosePlacement) {
    DockingManager dm;
    auto initial = DockNode::MakeLeaf();
    initial->AddTab("Assets");
    initial->AddTab("Log");
    dm.SetRoot(std::move(initial));
    ASSERT_TRUE(dm.RemoveTab("Log"));

    auto replacement = DockNode::MakeLeaf();
    replacement->AddTab("SceneView");
    dm.SetRoot(std::move(replacement));

    EXPECT_FALSE(dm.RestoreLastClosedTab("Log"));
}

TEST(DockLayoutController, CloneAndPruneDropsUnregisteredTabsAndEmptyBranches) {
    UIElement panelA("panel-a");
    DockingManager dm;
    dm.RegisterPanel("A", &panelA);

    auto source = DockNode::MakeSplit(DockPosition::Left, 0.3f);
    source->First()->AddTab("A");
    source->First()->AddTab("MissingTab");
    ASSERT_TRUE(source->First()->ActivateTab("MissingTab"));
    source->Second()->AddTab("MissingBranch");

    DockLayoutController controller(dm);
    auto pruned = controller.CloneForRegisteredPanels(source.get());

    ASSERT_NE(pruned, nullptr);
    ASSERT_TRUE(pruned->IsLeaf());
    ASSERT_EQ(pruned->GetTabs().size(), 1u);
    EXPECT_EQ(pruned->GetTabs()[0], "A");
    EXPECT_EQ(pruned->GetActivePanelId(), "A");

    // Cloning must not mutate the stored preset.
    ASSERT_TRUE(source->IsSplit());
    EXPECT_EQ(source->First()->GetTabs().size(), 2u);
    EXPECT_EQ(source->Second()->GetTabs().size(), 1u);
}

TEST(DockLayoutController, CloneAndPrunePreservesSurvivingSplitGeometry) {
    UIElement panelA("panel-a");
    UIElement panelB("panel-b");
    DockingManager dm;
    dm.RegisterPanel("A", &panelA);
    dm.RegisterPanel("B", &panelB);

    auto source = DockNode::MakeSplit(DockPosition::Bottom, 0.65f);
    source->SetMinChildSizes(140.0f, 180.0f);
    source->First()->AddTab("A");
    source->Second()->AddTab("Missing");
    source->Second()->AddTab("B");
    ASSERT_TRUE(source->Second()->ActivateTab("B"));

    DockLayoutController controller(dm);
    auto pruned = controller.CloneForRegisteredPanels(source.get());

    ASSERT_NE(pruned, nullptr);
    ASSERT_TRUE(pruned->IsSplit());
    EXPECT_EQ(pruned->GetSplitDirection(), DockPosition::Bottom);
    EXPECT_FLOAT_EQ(pruned->GetSplitRatio(), 0.65f);
    EXPECT_FLOAT_EQ(pruned->GetMinFirstPx(), 140.0f);
    EXPECT_FLOAT_EQ(pruned->GetMinSecondPx(), 180.0f);
    ASSERT_NE(pruned->First(), nullptr);
    ASSERT_NE(pruned->Second(), nullptr);
    EXPECT_EQ(pruned->First()->GetActivePanelId(), "A");
    EXPECT_EQ(pruned->Second()->GetActivePanelId(), "B");
}

TEST(DockLayoutController, ApplyPrunedLayoutRejectsAnEntirelyUnavailablePreset) {
    UIElement currentPanel("current-panel");
    DockingManager dm;
    dm.RegisterPanel("Current", &currentPanel);

    auto current = DockNode::MakeLeaf();
    current->AddTab("Current");
    dm.SetRoot(std::move(current));

    auto unavailable = DockNode::MakeLeaf();
    unavailable->AddTab("Missing");

    DockLayoutController controller(dm);
    EXPECT_FALSE(controller.ApplyForRegisteredPanels(unavailable.get()));
    ASSERT_NE(dm.GetRoot(), nullptr);
    EXPECT_EQ(dm.GetRoot()->GetActivePanelId(), "Current");
}

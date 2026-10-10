#include <gtest/gtest.h>

#include "UI/Controls/TreeView.h"

using namespace GameEngine;

namespace {

class TestTreeProvider : public TreeChangeTrackingProvider {
public:
    int GetRootCount() const override { return 1; }

    TreeId GetRootId(int index) const override {
        (void)index;
        return 1; // single root
    }

    int GetChildCount(TreeId parent) const override {
        if (parent == 1) return 2;      // children: 2, 3
        if (parent == 2) return 1;      // child: 4
        return 0;                       // leaves
    }

    TreeId GetChildId(TreeId parent, int index) const override {
        if (parent == 1) {
            return (index == 0) ? 2 : 3;
        }
        if (parent == 2) {
            return 4;
        }
        return 0;
    }

    const char* GetLabel(TreeId id) const override {
        switch (id) {
        case 1: return "root";
        case 2: return "child-a";
        case 3: return "child-b";
        case 4: return "grandchild";
        default: return "";
        }
    }

    bool IsExpandable(TreeId id) const override {
        // Only nodes with children are expandable
        return id == 1 || id == 2;
    }
};

} // namespace

TEST(TreeViewTests, ExpandAllExpandsAllExpandableNodes)
{
    TestTreeProvider provider;
    TreeView view;
    view.SetDataProvider(&provider);
    view.SetShowRoot(true);

    // Initially nothing is expanded
    EXPECT_FALSE(view.IsExpanded(1));
    EXPECT_FALSE(view.IsExpanded(2));
    EXPECT_FALSE(view.IsExpanded(3));
    EXPECT_FALSE(view.IsExpanded(4));

    view.ExpandAll();

    // All expandable nodes (those with children) should now be expanded
    EXPECT_TRUE(view.IsExpanded(1));
    EXPECT_TRUE(view.IsExpanded(2));

    // Leaf nodes should remain non-expandable/non-expanded
    EXPECT_FALSE(view.IsExpanded(3));
    EXPECT_FALSE(view.IsExpanded(4));
}


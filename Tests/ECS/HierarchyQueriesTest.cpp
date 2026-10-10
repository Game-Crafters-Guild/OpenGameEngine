#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"

#include <algorithm>

using namespace GameEngine::ECS;
using GameEngine::Components::Parent;
using GameEngine::Components::ParentRelationIndex;
using GameEngine::Components::ChildrenOf;
using GameEngine::Components::DescendantsOf;
using GameEngine::Components::IsDescendantOf;
using GameEngine::ECS::test::Position;

namespace {

static EntityHandle MakeNode(World& w, EntityHandle parent = {}) {
    auto e = w.Create();
    e.Set(Position{0, 0, 0});
    if (parent.IsValid()) e.Set(Parent{parent});
    return e.GetHandle();
}

static bool Contains(const std::vector<EntityHandle>& v, EntityHandle e) {
    return std::find(v.begin(), v.end(), e) != v.end();
}

} // namespace

TEST(HierarchyQueries, ChildrenOf_ReturnsDirectChildrenOnly) {
    World w;
    auto root = MakeNode(w);
    auto childA = MakeNode(w, root);
    auto childB = MakeNode(w, root);
    auto grandchild = MakeNode(w, childA);
    auto unrelated = MakeNode(w);
    w.ProcessCommands();

    auto kids = ChildrenOf(w, root);

    EXPECT_EQ(kids.size(), 2u);
    EXPECT_TRUE(Contains(kids, childA));
    EXPECT_TRUE(Contains(kids, childB));
    EXPECT_FALSE(Contains(kids, grandchild));
    EXPECT_FALSE(Contains(kids, unrelated));
}

TEST(HierarchyQueries, ChildrenOf_OutParamReusesBuffer) {
    World w;
    auto root = MakeNode(w);
    MakeNode(w, root);
    MakeNode(w, root);
    w.ProcessCommands();

    std::vector<EntityHandle> buf;
    buf.reserve(64);
    const auto capacityBefore = buf.capacity();
    ChildrenOf(w, root, buf);
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_GE(buf.capacity(), capacityBefore);
}

TEST(HierarchyQueries, ChildrenOf_InvalidParentReturnsEmpty) {
    World w;
    MakeNode(w);
    w.ProcessCommands();

    auto kids = ChildrenOf(w, EntityHandle::Invalid());
    EXPECT_TRUE(kids.empty());
}

TEST(HierarchyQueries, DescendantsOf_WalksEntireSubtree) {
    World w;
    auto root = MakeNode(w);
    auto childA = MakeNode(w, root);
    auto childB = MakeNode(w, root);
    auto grandA = MakeNode(w, childA);
    auto grandB = MakeNode(w, childA);
    auto greatGrand = MakeNode(w, grandA);
    auto unrelated = MakeNode(w);
    w.ProcessCommands();

    auto desc = DescendantsOf(w, root);

    EXPECT_EQ(desc.size(), 5u);
    EXPECT_TRUE(Contains(desc, childA));
    EXPECT_TRUE(Contains(desc, childB));
    EXPECT_TRUE(Contains(desc, grandA));
    EXPECT_TRUE(Contains(desc, grandB));
    EXPECT_TRUE(Contains(desc, greatGrand));
    EXPECT_FALSE(Contains(desc, root));
    EXPECT_FALSE(Contains(desc, unrelated));
}

TEST(HierarchyQueries, DescendantsOf_EmptyForLeaf) {
    World w;
    auto root = MakeNode(w);
    auto leaf = MakeNode(w, root);
    w.ProcessCommands();

    auto desc = DescendantsOf(w, leaf);
    EXPECT_TRUE(desc.empty());
}

TEST(HierarchyQueries, IsDescendantOf_DirectAndIndirect) {
    World w;
    auto root = MakeNode(w);
    auto mid = MakeNode(w, root);
    auto leaf = MakeNode(w, mid);
    auto sibling = MakeNode(w, root);
    w.ProcessCommands();

    EXPECT_TRUE(IsDescendantOf(w, mid, root));
    EXPECT_TRUE(IsDescendantOf(w, leaf, root));
    EXPECT_TRUE(IsDescendantOf(w, leaf, mid));
    EXPECT_TRUE(IsDescendantOf(w, sibling, root));

    EXPECT_FALSE(IsDescendantOf(w, root, leaf));
    EXPECT_FALSE(IsDescendantOf(w, mid, leaf));
    EXPECT_FALSE(IsDescendantOf(w, sibling, mid));
    EXPECT_FALSE(IsDescendantOf(w, root, root));
}

TEST(HierarchyQueries, IsDescendantOf_UnrelatedHandleReturnsFalse) {
    World w;
    auto root = MakeNode(w);
    auto mid = MakeNode(w, root);
    auto child = MakeNode(w, mid);
    w.ProcessCommands();

    // A handle that does not appear anywhere in child's ancestry must not match.
    EXPECT_FALSE(IsDescendantOf(w, child, EntityHandle(999u, 0)));
}

TEST(HierarchyQueries, DescendantsOf_TerminatesOnCycle) {
    World w;
    auto a = MakeNode(w);
    auto b = MakeNode(w);
    w.ProcessCommands();

    // Close the cycle a -> b -> a so DescendantsOf would infinite-loop without a visited set.
    w.AddComponentImmediate(a, Parent{b});
    w.AddComponentImmediate(b, Parent{a});
    w.ProcessCommands();

    auto desc = DescendantsOf(w, a);
    // With the cycle, reachable-from-a is just {b}: b is a child of a (has Parent{a}).
    // The back-edge from a to b would push a again, but visited-set skips it.
    EXPECT_EQ(desc.size(), 1u);
    EXPECT_TRUE(Contains(desc, b));
}

TEST(HierarchyQueries, IsDescendantOf_BoundedByDepthOnCycle) {
    // Manufacture a cycle by swapping Parent pointers. The depth cap must
    // return false rather than spinning forever.
    World w;
    auto a = MakeNode(w);
    auto b = MakeNode(w);
    w.ProcessCommands();

    // Close the cycle: a -> b -> a
    w.AddComponentImmediate(a, Parent{b});
    w.AddComponentImmediate(b, Parent{a});

    auto unrelated = MakeNode(w);
    w.ProcessCommands();

    EXPECT_FALSE(IsDescendantOf(w, a, unrelated));
    EXPECT_FALSE(IsDescendantOf(w, b, unrelated));
}

TEST(HierarchyQueries, IsDescendantOf_DepthCapReturnsFalseBeyondLimit) {
    // Build a chain deeper than kMaxHierarchyDepth. The farthest ancestor is
    // genuinely reachable, but the cap should give up before finding it.
    using GameEngine::Components::kMaxHierarchyDepth;
    World w;
    auto root = MakeNode(w);
    EntityHandle cur = root;
    for (std::size_t i = 1; i < kMaxHierarchyDepth + 10; ++i) {
        cur = MakeNode(w, cur);
    }
    w.ProcessCommands();

    // Walk 100 up from leaf — far inside the cap — and check that ancestor is found.
    EntityHandle probe = cur;
    for (int i = 0; i < 100; ++i) {
        auto* p = w.GetComponent<Parent>(probe);
        if (!p || !p->parent.IsValid()) break;
        probe = p->parent;
    }
    EXPECT_TRUE(IsDescendantOf(w, cur, probe));

    // The true root is beyond the cap, so the helper gives up.
    EXPECT_FALSE(IsDescendantOf(w, cur, root));
}

// ---------------------------------------------------------------------------
// Indexed overloads — must produce identical output to the stateless ones.
// ---------------------------------------------------------------------------

TEST(HierarchyQueries, ChildrenOf_Indexed_MatchesStateless) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    auto b = MakeNode(w, root);
    MakeNode(w, a); // grandchild, excluded
    MakeNode(w);    // unrelated
    w.ProcessCommands();

    std::vector<EntityHandle> stateless, indexed;
    ChildrenOf(w, root, stateless);

    ParentRelationIndex idx;
    ChildrenOf(w, root, idx, indexed);

    EXPECT_EQ(stateless.size(), indexed.size());
    for (auto e : stateless) EXPECT_TRUE(Contains(indexed, e));
    EXPECT_TRUE(Contains(indexed, a));
    EXPECT_TRUE(Contains(indexed, b));
}

TEST(HierarchyQueries, DescendantsOf_Indexed_MatchesStateless) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    auto b = MakeNode(w, root);
    auto gA = MakeNode(w, a);
    MakeNode(w, gA); // great-grandchild
    MakeNode(w);     // unrelated
    w.ProcessCommands();

    std::vector<EntityHandle> stateless, indexed;
    DescendantsOf(w, root, stateless);

    ParentRelationIndex idx;
    DescendantsOf(w, root, idx, indexed);

    EXPECT_EQ(stateless.size(), indexed.size());
    for (auto e : stateless) EXPECT_TRUE(Contains(indexed, e));
}

TEST(HierarchyQueries, DescendantsOf_Indexed_HandlesCycle) {
    World w;
    auto a = MakeNode(w);
    auto b = MakeNode(w);
    w.ProcessCommands();
    w.AddComponentImmediate(a, Parent{b});
    w.AddComponentImmediate(b, Parent{a});
    w.ProcessCommands();

    std::vector<EntityHandle> indexed;
    ParentRelationIndex idx;
    DescendantsOf(w, a, idx, indexed);
    // Cycle: from a we should reach b exactly once; visited-set prevents looping.
    EXPECT_EQ(indexed.size(), 1u);
    EXPECT_TRUE(Contains(indexed, b));
}

TEST(HierarchyQueries, DescendantsOf_Indexed_ReusesAcrossCalls) {
    // Same index instance across multiple queries — cache must not leak or
    // miss updates after a structural change.
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    w.ProcessCommands();

    ParentRelationIndex idx;
    std::vector<EntityHandle> r1;
    DescendantsOf(w, root, idx, r1);
    EXPECT_EQ(r1.size(), 1u);

    auto b = MakeNode(w, root);
    w.ProcessCommands();
    std::vector<EntityHandle> r2;
    DescendantsOf(w, root, idx, r2);
    EXPECT_EQ(r2.size(), 2u);
    EXPECT_TRUE(Contains(r2, a));
    EXPECT_TRUE(Contains(r2, b));
}

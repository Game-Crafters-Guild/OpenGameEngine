#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/RelationIndex.h"
#include "ECS/World.h"
#include "TestComponents.h"

#include <algorithm>
#include <random>

using namespace GameEngine::ECS;
using GameEngine::Components::Parent;
using GameEngine::ECS::test::Position;

namespace {

struct ParentOf {}; // relation tag
using ParentIndex = RelationIndex<ParentOf, Parent, &Parent::parent>;

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

TEST(RelationIndex, ReturnsNullForUnparentedTarget) {
    World w;
    auto root = MakeNode(w);
    w.ProcessCommands();

    ParentIndex idx;
    EXPECT_EQ(idx.GetSources(w, root), nullptr);
}

TEST(RelationIndex, FindsDirectChildren) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    auto b = MakeNode(w, root);
    auto grand = MakeNode(w, a); // not a direct child of root
    w.ProcessCommands();

    ParentIndex idx;
    const auto* children = idx.GetSources(w, root);
    ASSERT_NE(children, nullptr);
    EXPECT_EQ(children->size(), 2u);
    EXPECT_TRUE(Contains(*children, a));
    EXPECT_TRUE(Contains(*children, b));
    EXPECT_FALSE(Contains(*children, grand));
}

TEST(RelationIndex, RebuildsAfterStructuralChange) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    w.ProcessCommands();

    ParentIndex idx;
    EXPECT_EQ(idx.CountSources(w, root), 1u);

    // Add another child — structural change invalidates the cache.
    auto b = MakeNode(w, root);
    w.ProcessCommands();

    EXPECT_EQ(idx.CountSources(w, root), 2u);
    const auto* children = idx.GetSources(w, root);
    ASSERT_NE(children, nullptr);
    EXPECT_TRUE(Contains(*children, a));
    EXPECT_TRUE(Contains(*children, b));
}

TEST(RelationIndex, RebuildsAfterRemove) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    auto b = MakeNode(w, root);
    w.ProcessCommands();

    ParentIndex idx;
    EXPECT_EQ(idx.CountSources(w, root), 2u);

    w.DestroyEntityImmediate(a);

    EXPECT_EQ(idx.CountSources(w, root), 1u);
    const auto* children = idx.GetSources(w, root);
    ASSERT_NE(children, nullptr);
    EXPECT_TRUE(Contains(*children, b));
    EXPECT_FALSE(Contains(*children, a));
}

TEST(RelationIndex, HandlesManyRoots) {
    World w;
    std::vector<EntityHandle> roots;
    for (int i = 0; i < 50; ++i) roots.push_back(MakeNode(w));
    // 10 children per root, round-robin.
    for (int i = 0; i < 500; ++i) MakeNode(w, roots[i % 50]);
    w.ProcessCommands();

    ParentIndex idx;
    for (auto r : roots) {
        EXPECT_EQ(idx.CountSources(w, r), 10u);
    }
}

TEST(RelationIndex, IgnoresEntitiesWithInvalidParent) {
    World w;
    auto root = MakeNode(w);
    auto a = MakeNode(w, root);
    // Explicitly set an invalid Parent on a fresh entity — should not appear
    // under any target's child list.
    auto orphan = w.Create();
    orphan.Set(Parent{EntityHandle{}}); // default-constructed == invalid
    w.ProcessCommands();

    ParentIndex idx;
    const auto* children = idx.GetSources(w, root);
    ASSERT_NE(children, nullptr);
    EXPECT_EQ(children->size(), 1u);
    EXPECT_TRUE(Contains(*children, a));
    EXPECT_FALSE(Contains(*children, orphan.GetHandle()));
}

TEST(RelationIndex, InvalidateForcesRebuild) {
    World w;
    auto root = MakeNode(w);
    MakeNode(w, root);
    w.ProcessCommands();

    ParentIndex idx;
    EXPECT_EQ(idx.CountSources(w, root), 1u);
    idx.Invalidate();
    // Must still get the correct count — rebuild happens transparently.
    EXPECT_EQ(idx.CountSources(w, root), 1u);
}

// ---------------------------------------------------------------------------
// Entity-aware hook smoke test — Phase 2 primitive. Proves the plumbing
// threads the source entity through to the callback. Full RelationIndex
// wiring via hooks is deferred.
// ---------------------------------------------------------------------------
namespace {
struct CapturedHook {
    std::vector<EntityHandle> SeenEntities;
    std::vector<EntityHandle> SeenTargets;
};
static CapturedHook g_Hook; // per-test scratch; tests below are sequential

static void OnParentSet(EntityHandle e, Parent& p) {
    g_Hook.SeenEntities.push_back(e);
    g_Hook.SeenTargets.push_back(p.parent);
}
static void OnParentRemoved(EntityHandle e, Parent& p) {
    g_Hook.SeenEntities.push_back(e);
    g_Hook.SeenTargets.push_back(p.parent);
}
} // namespace

TEST(RelationIndex, EntityAwareHookReceivesSourceHandle) {
    g_Hook = {};
    World w;
    w.RegisterComponents<Parent>();
    w.RegisterOnSet<Parent>(&OnParentSet);

    auto root = MakeNode(w);
    auto child = MakeNode(w, root);
    w.ProcessCommands();

    ASSERT_EQ(g_Hook.SeenEntities.size(), 1u);
    EXPECT_EQ(g_Hook.SeenEntities[0], child);
    EXPECT_EQ(g_Hook.SeenTargets[0], root);
}

TEST(RelationIndex, EntityAwareOnRemoveReceivesSourceHandle) {
    g_Hook = {};
    World w;
    w.RegisterComponents<Parent>();
    w.RegisterOnRemove<Parent>(&OnParentRemoved);

    auto root = MakeNode(w);
    auto child = MakeNode(w, root);
    w.ProcessCommands();

    w.DestroyEntityImmediate(child);

    ASSERT_EQ(g_Hook.SeenEntities.size(), 1u);
    EXPECT_EQ(g_Hook.SeenEntities[0], child);
    EXPECT_EQ(g_Hook.SeenTargets[0], root);
}

TEST(RelationIndex, MatchesScanAcrossRandomMutations) {
    // Fuzz: random add/remove sequences must leave the index consistent with
    // a naive ground-truth scan over Parent components.
    std::mt19937 rng(1337);
    World w;
    std::vector<EntityHandle> roots;
    for (int i = 0; i < 10; ++i) roots.push_back(MakeNode(w));

    ParentIndex idx;
    std::vector<EntityHandle> alive;
    for (int step = 0; step < 200; ++step) {
        // 70% add, 30% remove (when there's something to remove)
        const bool add = alive.empty() || (std::uniform_int_distribution<int>(0, 9)(rng) < 7);
        if (add) {
            auto parent = roots[std::uniform_int_distribution<size_t>(0, roots.size() - 1)(rng)];
            alive.push_back(MakeNode(w, parent));
        } else {
            size_t i = std::uniform_int_distribution<size_t>(0, alive.size() - 1)(rng);
            w.DestroyEntityImmediate(alive[i]);
            alive.erase(alive.begin() + i);
        }
        w.ProcessCommands();

        // Verify every root's index matches a ground-truth scan.
        for (auto r : roots) {
            std::vector<EntityHandle> expected;
            w.Query<Read<Parent>>().Each([&](EntityHandle e, const Parent& p) {
                if (p.parent == r) expected.push_back(e);
            });
            const auto* got = idx.GetSources(w, r);
            const size_t gotSize = got ? got->size() : 0;
            ASSERT_EQ(gotSize, expected.size()) << "step=" << step << " root=" << r.index;
            for (auto e : expected) {
                ASSERT_TRUE(got && Contains(*got, e))
                    << "step=" << step << " missing entity=" << e.index;
            }
        }
    }
}

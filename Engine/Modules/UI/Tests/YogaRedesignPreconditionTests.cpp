// Preconditions for the primary-flip redesign: these pin the Yoga 3.x
// behaviours the redesign depends on, and cover the MountRegistry
// infrastructure it introduces.

#include <gtest/gtest.h>
#include "UI/Layout/YogaLayout.h"
#include "UI/ResolvedStyle.h"
#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

using namespace GameEngine;
using namespace GameEngine::UILayout;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA

namespace {

YGNodeRef MakeFixed(float w, float h)
{
    YGNodeRef n = YogaAdapter::CreateNode();
    ResolvedStyle s;
    s.Layout.Width = StyleLength::Px(w);
    s.Layout.Height = StyleLength::Px(h);
    YogaAdapter::ApplyStyle(n, s);
    return n;
}

YGNodeRef MakeContainer(float w, float h)
{
    YGNodeRef n = YogaAdapter::CreateNode();
    ResolvedStyle s;
    s.Layout.Width = StyleLength::Px(w);
    s.Layout.Height = StyleLength::Px(h);
    s.Layout.FlexDirection = FlexDirection::Row;
    s.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(n, s);
    return n;
}

} // namespace

// Step 0a: A YGNode can be moved from one Yoga tree to another via
// YGNodeRemoveChild + YGNodeInsertChild. Primary-flip Model A depends on
// this: UIElement's m_YogaNode follows the element across UIManagers
// without being destroyed and recreated.
TEST(YogaRedesignPrecondition, CrossRootReparentKeepsNodeAndLayoutResolves)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef rootA = MakeContainer(500.0f, 100.0f);
    YGNodeRef rootB = MakeContainer(300.0f, 100.0f);
    YGNodeRef child = MakeFixed(100.0f, 50.0f);

    // Start under A.
    YGNodeInsertChild(rootA, child, 0);
    YogaAdapter::CalculateLayout(rootA, 500.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(child), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetWidth(child), 100.0f, 0.01f);
    EXPECT_EQ(YGNodeGetOwner(child), rootA);

    // Move to B.
    YGNodeRemoveChild(rootA, child);
    EXPECT_EQ(YGNodeGetOwner(child), nullptr);
    EXPECT_EQ(YGNodeGetChildCount(rootA), 0u);

    YGNodeInsertChild(rootB, child, 0);
    EXPECT_EQ(YGNodeGetOwner(child), rootB);
    EXPECT_EQ(YGNodeGetChildCount(rootB), 1u);

    YogaAdapter::CalculateLayout(rootB, 300.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(child), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetWidth(child), 100.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetHeight(child), 50.0f, 0.01f);

    YogaAdapter::DestroyNode(child);
    YogaAdapter::DestroyNode(rootB);
    YogaAdapter::DestroyNode(rootA);
}

// Step 0a corollary: moving a subtree (not just a leaf) across roots
// preserves internal parent/child links in the subtree.
TEST(YogaRedesignPrecondition, CrossRootReparentPreservesSubtree)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef rootA = MakeContainer(500.0f, 100.0f);
    YGNodeRef rootB = MakeContainer(400.0f, 100.0f);
    YGNodeRef mid = MakeContainer(200.0f, 80.0f);
    YGNodeRef leaf = MakeFixed(80.0f, 40.0f);

    YGNodeInsertChild(mid, leaf, 0);
    YGNodeInsertChild(rootA, mid, 0);
    YogaAdapter::CalculateLayout(rootA, 500.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetWidth(leaf), 80.0f, 0.01f);

    // Move mid (with its leaf child) to B.
    YGNodeRemoveChild(rootA, mid);
    YGNodeInsertChild(rootB, mid, 0);

    EXPECT_EQ(YGNodeGetOwner(mid), rootB);
    EXPECT_EQ(YGNodeGetOwner(leaf), mid);
    EXPECT_EQ(YGNodeGetChildCount(mid), 1u);

    YogaAdapter::CalculateLayout(rootB, 400.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetWidth(leaf), 80.0f, 0.01f);

    YogaAdapter::DestroyNode(leaf);
    YogaAdapter::DestroyNode(mid);
    YogaAdapter::DestroyNode(rootB);
    YogaAdapter::DestroyNode(rootA);
}

// Step 0b: YGNodeFree (our YogaAdapter::DestroyNode) auto-detaches a node
// from its parent's child vector. This is what makes dtor-based lifetime
// safe under the new model — if a caller forgets an explicit RemoveChild,
// freeing doesn't corrupt the parent.
TEST(YogaRedesignPrecondition, DestroyNodeAutoDetachesFromOwner)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef parent = MakeContainer(400.0f, 100.0f);
    YGNodeRef c0 = MakeFixed(100.0f, 50.0f);
    YGNodeRef c1 = MakeFixed(100.0f, 50.0f);

    YGNodeInsertChild(parent, c0, 0);
    YGNodeInsertChild(parent, c1, 1);
    EXPECT_EQ(YGNodeGetChildCount(parent), 2u);

    // Free c0 without removing it first. Parent's child list must shrink.
    YogaAdapter::DestroyNode(c0);
    EXPECT_EQ(YGNodeGetChildCount(parent), 1u);
    EXPECT_EQ(YGNodeGetChild(parent, 0), c1);
    EXPECT_EQ(YGNodeGetOwner(c1), parent);

    // Solve still works.
    YogaAdapter::CalculateLayout(parent, 400.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c1), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetWidth(c1), 100.0f, 0.01f);

    YogaAdapter::DestroyNode(c1);
    YogaAdapter::DestroyNode(parent);
}

// Step 0b: reordering children via Remove + Insert does not duplicate or
// drop nodes. Sanity check that the InsertChildrenSortedByOrder pattern
// the redesign uses for display-flip reattach is well-defined.
TEST(YogaRedesignPrecondition, RemoveInsertReorderIsStable)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef parent = MakeContainer(600.0f, 100.0f);
    YGNodeRef a = MakeFixed(100.0f, 50.0f);
    YGNodeRef b = MakeFixed(100.0f, 50.0f);
    YGNodeRef c = MakeFixed(100.0f, 50.0f);

    YGNodeInsertChild(parent, a, 0);
    YGNodeInsertChild(parent, b, 1);
    YGNodeInsertChild(parent, c, 2);

    // Reorder: move c to index 0 → {c, a, b}.
    YGNodeRemoveChild(parent, c);
    YGNodeInsertChild(parent, c, 0);

    ASSERT_EQ(YGNodeGetChildCount(parent), 3u);
    EXPECT_EQ(YGNodeGetChild(parent, 0), c);
    EXPECT_EQ(YGNodeGetChild(parent, 1), a);
    EXPECT_EQ(YGNodeGetChild(parent, 2), b);

    YogaAdapter::CalculateLayout(parent, 600.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 100.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 200.0f, 0.01f);

    YogaAdapter::DestroyNode(a);
    YogaAdapter::DestroyNode(b);
    YogaAdapter::DestroyNode(c);
    YogaAdapter::DestroyNode(parent);
}

// Step 0b: detach-reattach at a different index exercises the display:none
// reattach path — element leaves the Yoga tree (displayMode=None), then
// comes back at the correct visible-only index.
TEST(YogaRedesignPrecondition, DetachReattachAtDifferentIndex)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef parent = MakeContainer(600.0f, 100.0f);
    YGNodeRef a = MakeFixed(100.0f, 50.0f);
    YGNodeRef b = MakeFixed(100.0f, 50.0f);
    YGNodeRef c = MakeFixed(100.0f, 50.0f);

    YGNodeInsertChild(parent, a, 0);
    YGNodeInsertChild(parent, b, 1);
    YGNodeInsertChild(parent, c, 2);

    // "b goes display:none": remove it. Parent shrinks to {a, c}.
    YGNodeRemoveChild(parent, b);
    ASSERT_EQ(YGNodeGetChildCount(parent), 2u);
    EXPECT_EQ(YGNodeGetOwner(b), nullptr);

    YogaAdapter::CalculateLayout(parent, 600.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c), 100.0f, 0.01f);

    // "b comes back visible" — reinsert at its visible-skipping index
    // (between a and c → index 1).
    YGNodeInsertChild(parent, b, 1);
    ASSERT_EQ(YGNodeGetChildCount(parent), 3u);
    EXPECT_EQ(YGNodeGetChild(parent, 0), a);
    EXPECT_EQ(YGNodeGetChild(parent, 1), b);
    EXPECT_EQ(YGNodeGetChild(parent, 2), c);

    YogaAdapter::CalculateLayout(parent, 600.0f, 100.0f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 100.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c), 200.0f, 0.01f);

    YogaAdapter::DestroyNode(a);
    YogaAdapter::DestroyNode(b);
    YogaAdapter::DestroyNode(c);
    YogaAdapter::DestroyNode(parent);
}

// Step 0b: measure function fires once per solve when child order changes.
// Verifies YGNodeMarkDirty semantics we rely on for the RebindMeasureFunc
// path under the new design.
namespace {
struct MeasureCallCounter
{
    int calls = 0;
    float width = 50.0f;
    float height = 20.0f;
};

YGSize MeasureFn(YGNodeConstRef node, float /*w*/, YGMeasureMode /*wm*/,
                 float /*h*/, YGMeasureMode /*hm*/)
{
    auto* ctx = reinterpret_cast<MeasureCallCounter*>(const_cast<void*>(YGNodeGetContext(node)));
    if (ctx) ctx->calls++;
    return YGSize{ctx ? ctx->width : 50.0f, ctx ? ctx->height : 20.0f};
}
} // namespace

TEST(YogaRedesignPrecondition, MeasureFnFiresAfterMarkDirtyAndReorder)
{
    if (!YogaAdapter::IsAvailable()) GTEST_SKIP() << "Yoga not available";

    YGNodeRef parent = MakeContainer(400.0f, 100.0f);
    YGNodeRef a = MakeFixed(100.0f, 50.0f);

    // Leaf measure node — no children.
    YGNodeRef m = YogaAdapter::CreateNode();
    MeasureCallCounter ctr;
    YGNodeSetContext(m, &ctr);
    YGNodeSetMeasureFunc(m, &MeasureFn);

    YGNodeInsertChild(parent, a, 0);
    YGNodeInsertChild(parent, m, 1);

    YogaAdapter::CalculateLayout(parent, 400.0f, 100.0f);
    EXPECT_GE(ctr.calls, 1);
    const int afterFirst = ctr.calls;

    // Change measure inputs; Yoga caches, so mark dirty explicitly.
    ctr.width = 120.0f;
    YGNodeMarkDirty(m);

    YogaAdapter::CalculateLayout(parent, 400.0f, 100.0f);
    EXPECT_GT(ctr.calls, afterFirst);
    EXPECT_NEAR(YGNodeLayoutGetWidth(m), 120.0f, 0.01f);

    // Reorder: move m before a. Measure fn should still be alive on m.
    YGNodeRemoveChild(parent, m);
    YGNodeInsertChild(parent, m, 0);
    ctr.calls = 0;
    ctr.width = 80.0f;
    YGNodeMarkDirty(m);

    YogaAdapter::CalculateLayout(parent, 400.0f, 100.0f);
    EXPECT_GE(ctr.calls, 1);
    EXPECT_NEAR(YGNodeLayoutGetWidth(m), 80.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(m), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 80.0f, 0.01f);

    YogaAdapter::DestroyNode(a);
    YogaAdapter::DestroyNode(m);
    YogaAdapter::DestroyNode(parent);
}

#endif // GE_HAVE_YOGA

// ---------------------------------------------------------------------------
// Step 0c: MountRegistry
// ---------------------------------------------------------------------------

// Registering a Mount by SetTarget populates the registry; re-setting to
// nullptr removes it.
TEST(MountRegistryTests, SetTargetRegistersAndUnregisters)
{
    const size_t before = MountRegistry::Instance().Size();

    auto target = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();

    mount->SetTarget(target.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);
    EXPECT_EQ(mount->GetTarget(), target.get());

    mount->SetTarget(nullptr);
    EXPECT_EQ(MountRegistry::Instance().Size(), before);
    EXPECT_EQ(mount->GetTarget(), nullptr);
}

// Changing target moves the registry entry; no leak of the old key.
TEST(MountRegistryTests, RetargetUpdatesKey)
{
    const size_t before = MountRegistry::Instance().Size();

    auto t1 = std::make_unique<UIElement>();
    auto t2 = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();

    mount->SetTarget(t1.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);

    mount->SetTarget(t2.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);
    EXPECT_EQ(mount->GetTarget(), t2.get());

    // Destroying t1 should be a no-op for this mount (it's targeting t2).
    MountRegistry::Instance().OnElementDestroyed(t1.get());
    EXPECT_EQ(mount->GetTarget(), t2.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);

    mount->SetTarget(nullptr);
}

// Destroying a Mount clears its entry even if the target is still alive.
TEST(MountRegistryTests, MountDestructionClearsEntry)
{
    const size_t before = MountRegistry::Instance().Size();
    auto target = std::make_unique<UIElement>();
    {
        auto mount = std::make_unique<Mount>();
        mount->SetTarget(target.get());
        EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);
    }
    EXPECT_EQ(MountRegistry::Instance().Size(), before);
}

// OnElementDestroyed nulls out matched mounts across the registry.
TEST(MountRegistryTests, OnElementDestroyedClearsMatchedMounts)
{
    const size_t before = MountRegistry::Instance().Size();

    auto victim = std::make_unique<UIElement>();
    auto bystander = std::make_unique<UIElement>();

    auto mount1 = std::make_unique<Mount>();
    auto mount2 = std::make_unique<Mount>();
    auto mount3 = std::make_unique<Mount>();

    mount1->SetTarget(victim.get());
    mount2->SetTarget(victim.get());
    mount3->SetTarget(bystander.get());
    // Second SetTarget on the same victim steals the target from mount1.
    EXPECT_EQ(mount1->GetTarget(), nullptr);
    EXPECT_EQ(mount2->GetTarget(), victim.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 2);

    // Destroy victim.
    MountRegistry::Instance().OnElementDestroyed(victim.get());

    EXPECT_EQ(mount1->GetTarget(), nullptr);
    EXPECT_EQ(mount2->GetTarget(), nullptr);
    EXPECT_EQ(mount3->GetTarget(), bystander.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);

    mount3->SetTarget(nullptr);
}

// SwapTargetForActivation updates the registry the same way as SetTarget.
TEST(MountRegistryTests, SwapTargetForActivationUpdatesRegistry)
{
    const size_t before = MountRegistry::Instance().Size();

    auto t1 = std::make_unique<UIElement>();
    auto t2 = std::make_unique<UIElement>();
    auto mount = std::make_unique<Mount>();

    mount->SwapTargetForActivation(t1.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);

    mount->SwapTargetForActivation(t2.get());
    EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);

    MountRegistry::Instance().OnElementDestroyed(t2.get());
    EXPECT_EQ(mount->GetTarget(), nullptr);
    EXPECT_EQ(MountRegistry::Instance().Size(), before);
}

// Step 2a: ~UIElement automatically notifies MountRegistry, so a Mount
// targeting an element that goes out of scope is cleaned up without
// any explicit OnElementDestroyed call.
TEST(MountRegistryTests, UIElementDestructionAutoClearsMountTarget)
{
    const size_t before = MountRegistry::Instance().Size();

    auto mount = std::make_unique<Mount>();
    {
        auto target = std::make_unique<UIElement>();
        mount->SetTarget(target.get());
        EXPECT_EQ(mount->GetTarget(), target.get());
        EXPECT_EQ(MountRegistry::Instance().Size(), before + 1);
        // target's unique_ptr destructs at end of scope, ~UIElement runs.
    }
    EXPECT_EQ(mount->GetTarget(), nullptr);
    EXPECT_EQ(MountRegistry::Instance().Size(), before);
}

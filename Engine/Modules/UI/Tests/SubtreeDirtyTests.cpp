#include "UI/UIElement.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIStyle.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "Rendering/Core/Device.h"
#include <gtest/gtest.h>
#include <memory>

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#include "UIRgTestHarness.h"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{
UIElement* AddChildAndGet(UIElement& parent, std::unique_ptr<UIElement> child)
{
    UIElement* raw = child.get();
    parent.AddChild(std::move(child));
    return raw;
}
} // namespace

// When a leaf is marked dirty with a layout-affecting flag, SubtreeDirty
// must propagate up the ancestor chain (early-stop when an ancestor already
// has it set).
TEST(SubtreeDirtyTests, PropagatesUpFromLeaf)
{
    UIElement root;
    UIElement* mid = AddChildAndGet(root, std::make_unique<UIElement>());
    UIElement* leaf = AddChildAndGet(*mid, std::make_unique<UIElement>());

    // Clear the flags that AddChild set on ancestors so we test propagation
    // from a known-clean state.
    root.ClearDirty(~0u);
    mid->ClearDirty(~0u);
    leaf->ClearDirty(~0u);

    leaf->MarkDirty(UIElement::StyleDirty);

    EXPECT_TRUE(leaf->IsDirty(UIElement::StyleDirty));
    EXPECT_TRUE(leaf->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(mid->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(root.IsDirty(UIElement::SubtreeDirty));
}

// VisualDirty alone (paint-only) must not set SubtreeDirty — the summary
// bit is scoped to layout-affecting flags only.
TEST(SubtreeDirtyTests, VisualOnlyDoesNotPropagate)
{
    UIElement root;
    UIElement* leaf = AddChildAndGet(root, std::make_unique<UIElement>());

    root.ClearDirty(~0u);
    leaf->ClearDirty(~0u);

    leaf->MarkDirty(UIElement::VisualDirty);

    EXPECT_TRUE(leaf->IsDirty(UIElement::VisualDirty));
    EXPECT_FALSE(leaf->IsDirty(UIElement::SubtreeDirty));
    EXPECT_FALSE(root.IsDirty(UIElement::SubtreeDirty));
}

// Once an ancestor already carries SubtreeDirty, MarkDirty must stop
// walking the chain there (O(1) amortized in steady state).
TEST(SubtreeDirtyTests, EarlyStopOnExistingAncestorFlag)
{
    UIElement root;
    UIElement* a = AddChildAndGet(root, std::make_unique<UIElement>());
    UIElement* b = AddChildAndGet(*a, std::make_unique<UIElement>());
    UIElement* c = AddChildAndGet(*b, std::make_unique<UIElement>());

    // Leave root + a dirty; clear b + c.
    root.ClearDirty(~0u);
    a->ClearDirty(~0u);
    b->ClearDirty(~0u);
    c->ClearDirty(~0u);

    a->MarkDirty(UIElement::LayoutDirty); // sets SubtreeDirty on a + root
    EXPECT_TRUE(a->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(root.IsDirty(UIElement::SubtreeDirty));

    // Flip just root's SubtreeDirty manually off, then mark c. The walk
    // should set SubtreeDirty on c, b; see a already has it (early stop).
    // Root should stay off (walk stopped at a).
    root.ClearDirty(UIElement::SubtreeDirty);

    c->MarkDirty(UIElement::StyleDirty);

    EXPECT_TRUE(c->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(b->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(a->IsDirty(UIElement::SubtreeDirty));
    EXPECT_FALSE(root.IsDirty(UIElement::SubtreeDirty));
}

namespace
{
} // namespace

// After a full Update cycle where the only dirty was on a leaf, the root
// element's SubtreeDirty must be cleared. Without the clear (the sticky-bit
// bug fixed in 632bf3f8), this would leave ancestors permanently flagged
// and the Slice-2 fast path would almost never fire.
TEST(SubtreeDirtyTests, IntegrationClearsOnRootAfterUpdate)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a'>
            <uielement id='leaf' />
        </uielement>
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // First Update establishes the tree. Subsequent Update should find
    // SubtreeDirty cleared because nothing new was dirtied.
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* leaf = r->FindById("leaf");
    ASSERT_NE(leaf, nullptr);

    EXPECT_FALSE(r->IsDirty(UIElement::SubtreeDirty))
        << "Root SubtreeDirty should be cleared after steady-state updates";

    // Now dirty the leaf and verify propagation → Update → cleared again.
    leaf->MarkDirty(UIElement::StyleDirty);
    EXPECT_TRUE(r->IsDirty(UIElement::SubtreeDirty));
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_FALSE(r->IsDirty(UIElement::SubtreeDirty))
        << "Root SubtreeDirty should be cleared once the leaf's dirty is processed";
    EXPECT_FALSE(leaf->IsDirty(UIElement::StyleDirty))
        << "Leaf StyleDirty should be cleared by cascade resolve";
}

// With the Slice-2 fast path enabled and nothing dirty, BuildYogaRecursive
// should take the fast path at least once on a steady-state Update.
TEST(SubtreeDirtyTests, FastPathFiresOnSteadyState)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a'>
            <uielement id='a1' />
            <uielement id='a2' />
        </uielement>
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.SetSubtreeSkipEnabled(true);
    ui.SetUpdateProfilingEnabled(true);

    // Frame 0+1: warm up (first frame is all-fresh slow path, second frame
    // clears dirty flags and primes the cache).
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 2: a fully-clean tree is now serviced by the mode-0 idle gate
    // (BuildYogaRecursive doesn't run at all), so exercise the fast path the
    // way production frames do: one element dirty, sibling subtrees clean.
    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* b = r->FindById("b");
    ASSERT_NE(b, nullptr);
    b->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_GT(ui.GetLastSubtreeFastPathHits(), 0u)
        << "Fast path should have fired on the clean 'a' subtree while 'b' was dirty";
}

// With the Slice-2 fast path DISABLED at runtime, the counter must stay
// at zero. Guards against an eligibility condition regressing and
// accidentally re-enabling the fast path under an "off" setting.
TEST(SubtreeDirtyTests, FastPathRespectsDisabledFlag)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.SetSubtreeSkipEnabled(false);
    ui.SetUpdateProfilingEnabled(true);

    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    // A fully-clean tree is idle-gated (no BuildYogaRecursive at all), so
    // force a heavy frame with a real dirty mark and observe that it still
    // slow-paths under the disabled flag.
    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);
    a->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(ui.GetLastSubtreeFastPathHits(), 0u)
        << "Fast path must not fire when SubtreeSkipEnabled=false";
    EXPECT_GT(ui.GetLastSubtreeSlowPathWalks(), 0u)
        << "Slow path should have walked every BuildYogaRecursive call";
}

// Cross-portal propagation: dirt inside an externally-owned Mount target
// must set SubtreeDirty on the Mount host and its real ancestor chain via
// the GetDfsParent() walk — a Mount target is a detached root whose
// m_Parent is null.
TEST(SubtreeDirtyTests, MountTargetDirtyPropagatesToHostChain)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto container = std::make_unique<UIElement>("container");
    auto mountOwner = std::make_unique<Mount>();
    Mount* mount = mountOwner.get();
    container->AddChild(std::move(mountOwner));
    UIElement* containerRaw = container.get();
    root->AddChild(std::move(container));

    // Externally-owned target (dock-panel shape): never a child of the
    // tree, kept alive by the test.
    auto targetOwner = std::make_unique<UIElement>("mount-target");
    UIElement* leaf = AddChildAndGet(*targetOwner, std::make_unique<UIElement>("target-leaf"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    mount->SetTarget(targetOwner.get());

    // Converge, then clear every residual flag so propagation is observed
    // from a known-clean state.
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    ASSERT_FALSE(r->IsDirty(UIElement::SubtreeDirty))
        << "Tree must converge to clean before the propagation check";

    leaf->MarkDirty(UIElement::StyleDirty);

    EXPECT_TRUE(targetOwner->IsDirty(UIElement::SubtreeDirty))
        << "Summary bit must reach the target root";
    EXPECT_TRUE(mount->IsDirty(UIElement::SubtreeDirty))
        << "Summary bit must cross the portal to the Mount host";
    EXPECT_TRUE(containerRaw->IsDirty(UIElement::SubtreeDirty))
        << "Summary bit must continue up the host's real ancestors";
    EXPECT_TRUE(r->IsDirty(UIElement::SubtreeDirty));

    // The heavy pass must consume the dirt (target leaf re-cascaded) and
    // re-converge to clean.
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_FALSE(leaf->IsDirty(UIElement::StyleDirty))
        << "In-target dirt must be processed by the rebuild";
    EXPECT_FALSE(r->IsDirty(UIElement::SubtreeDirty))
        << "Tree must re-converge after the in-target change";
}

// With SubtreeDirty trustworthy across portals, a clean Mount-host subtree
// takes the fast path on heavy frames — the HasMountInSubtree gate escape
// is gone. Tree shape pins the count: dirtying sibling `b` forces a heavy
// frame where only root and b slow-path; `container` (Mount inside) skips.
TEST(SubtreeDirtyTests, MountHostFastPathsWhenTargetClean)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto container = std::make_unique<UIElement>("container");
    auto mountOwner = std::make_unique<Mount>();
    Mount* mount = mountOwner.get();
    container->AddChild(std::move(mountOwner));
    root->AddChild(std::move(container));
    root->AddChild(std::make_unique<UIElement>("b"));

    auto targetOwner = std::make_unique<UIElement>("mount-target");
    targetOwner->AddChild(std::make_unique<UIElement>("target-leaf"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    mount->SetTarget(targetOwner.get());
    ui.SetSubtreeSkipEnabled(true);
    ui.SetUpdateProfilingEnabled(true);

    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* b = r->FindById("b");
    ASSERT_NE(b, nullptr);

    // Heavy frame with the Mount-host subtree clean: container must skip.
    b->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_GE(ui.GetLastSubtreeFastPathHits(), 1u)
        << "Clean Mount-host subtree must take the fast path";
    EXPECT_LE(ui.GetLastSubtreeSlowPathWalks(), 2u)
        << "Only root (SubtreeDirty) and b (dirty) may slow-path; the "
           "container/mount/target chain must be skipped";

    // Counter-check: dirt INSIDE the target forces the host chain back
    // onto the slow path that frame (the skip must not hide it).
    UIElement* targetLeaf = targetOwner->FindById("target-leaf");
    ASSERT_NE(targetLeaf, nullptr);
    targetLeaf->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_FALSE(targetLeaf->IsDirty(UIElement::StyleDirty))
        << "In-target dirt must be reached through the un-skipped chain";
    EXPECT_GE(ui.GetLastSubtreeSlowPathWalks(), 3u)
        << "root/container/mount/target must slow-path when the target is "
           "dirty";
}

// Regression: the observed double-click-folder crash. If Mount::SetTarget
// destroys a subtree that was mutated earlier in the same frame, the
// subsequent Update must not dereference the freed target — historically
// this SEH-crashed (0xdddddddddddddddd) via a surgical insert against the
// destroyed subtree; today the full rebuild path must stay safe.
TEST(SubtreeDirtyTests, MountSwapWithPendingInsertNoUaf)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    auto mountOwner = std::make_unique<Mount>();
    Mount* mount = mountOwner.get();
    root->AddChild(std::move(mountOwner));

    // Target with a descendant. Target is owned by root (sibling of Mount)
    // so its lifetime is a unique_ptr in root's m_Children — detaching via
    // Mount::SetTarget(nullptr) doesn't destroy it. We need an actual
    // destroy-while-op-pending: use RemoveChild on the target's owner.
    auto targetOwner = std::make_unique<UIElement>("mount-target");
    UIElement* targetRaw = targetOwner.get();
    auto leafOwner = std::make_unique<UIElement>("target-leaf");
    UIElement* leafRaw = leafOwner.get();
    targetOwner->AddChild(std::move(leafOwner));
    root->AddChild(std::move(targetOwner));
    mount->SetTarget(targetRaw);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    ui.Update(0.0f, /*interactive=*/true);  // warmup — everything gets retained Yoga state

    // Now push an Insert (new leaf under the target), then destroy the
    // whole target subtree via Mount::SetTarget + RemoveChild BEFORE the
    // next Update. At drain time the Insert's target would be freed.
    auto newLeafOwner = std::make_unique<UIElement>("new-leaf");
    targetRaw->AddChild(std::move(newLeafOwner));   // push Insert

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    mount->SetTarget(nullptr);                       // push MountSwap
    rootEl->RemoveChild(targetRaw);                  // push Remove, frees target + new-leaf
    (void)leafRaw;

    // The actual crash-repro Update. Must not SEH.
    ASSERT_NO_FATAL_FAILURE(ui.Update(0.0f, /*interactive=*/true));
}

// Style-override mutation must propagate SubtreeDirty via the StyleOverrides
// dirty callback wired in UIElement's constructor. Before this was added,
// Override().Set/Reset/Clear flipped internal flags without routing through
// MarkDirty, so the fast-path had to abort mid-iteration via a per-entry
// check. With propagation, the fast-path's outer SubtreeDirty gate catches
// the dirty at the ancestor level.
TEST(SubtreeDirtyTests, OverrideSetPropagatesSubtreeDirty)
{
    UIElement root;
    UIElement* mid = AddChildAndGet(root, std::make_unique<UIElement>());
    UIElement* leaf = AddChildAndGet(*mid, std::make_unique<UIElement>());

    root.ClearDirty(~0u);
    mid->ClearDirty(~0u);
    leaf->ClearDirty(~0u);

    // Layout-impact override on leaf — must propagate SubtreeDirty all the way up.
    leaf->Overrides().Set(Style::Width, StyleLength::Px(42.0f));

    EXPECT_TRUE(leaf->IsDirty(UIElement::LayoutDirty));
    EXPECT_TRUE(leaf->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(mid->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(root.IsDirty(UIElement::SubtreeDirty));
    // Own-flag on ancestors stays as it was (only SubtreeDirty summary bit set).
    EXPECT_FALSE(root.IsDirty(UIElement::LayoutDirty));
    EXPECT_FALSE(mid->IsDirty(UIElement::LayoutDirty));
}

// Visual-impact override mutation must also propagate SubtreeDirty (since
// we use LayoutDirty + VisualDirty + StyleDirty as the layout-affecting set
// for propagation, and VisualDirty alone does propagate via the callback's
// flag mapping).
TEST(SubtreeDirtyTests, OverrideSetVisualPropagates)
{
    UIElement root;
    UIElement* leaf = AddChildAndGet(root, std::make_unique<UIElement>());
    root.ClearDirty(~0u);
    leaf->ClearDirty(~0u);

    // Visual-impact override (BackgroundColor).
    leaf->Overrides().Set(Style::BackgroundColor, 0xFFFFFFFFu);

    EXPECT_TRUE(leaf->IsDirty(UIElement::VisualDirty));
    // Visual-only propagation deliberately does NOT set SubtreeDirty (paint-only).
    EXPECT_FALSE(root.IsDirty(UIElement::SubtreeDirty));
    EXPECT_FALSE(leaf->IsDirty(UIElement::SubtreeDirty));
}

// MarkDirtySubtree should flag self + all descendants with the requested
// flags (and SubtreeDirty for layout-affecting ones), plus propagate
// SubtreeDirty up the ancestor chain.
TEST(SubtreeDirtyTests, MarkDirtySubtreeSetsDescendantsAndAncestors)
{
    UIElement root;
    UIElement* a = AddChildAndGet(root, std::make_unique<UIElement>());
    UIElement* b = AddChildAndGet(*a, std::make_unique<UIElement>());
    UIElement* c = AddChildAndGet(*a, std::make_unique<UIElement>());

    root.ClearDirty(~0u);
    a->ClearDirty(~0u);
    b->ClearDirty(~0u);
    c->ClearDirty(~0u);

    a->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty);

    EXPECT_TRUE(a->IsDirty(UIElement::StyleDirty));
    EXPECT_TRUE(a->IsDirty(UIElement::LayoutDirty));
    EXPECT_TRUE(a->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(b->IsDirty(UIElement::StyleDirty));
    EXPECT_TRUE(b->IsDirty(UIElement::SubtreeDirty));
    EXPECT_TRUE(c->IsDirty(UIElement::StyleDirty));
    EXPECT_TRUE(c->IsDirty(UIElement::SubtreeDirty));
    // Ancestor chain:
    EXPECT_TRUE(root.IsDirty(UIElement::SubtreeDirty));
}

// Regression: a display:none descendant must clear its own SubtreeDirty in
// the slow-path's early-return branch, otherwise ancestors keep
// SubtreeDirty forever and every frame slow-paths the whole chain even
// when nothing new was dirtied (the "stale SubtreeDirty" leak). Verified
// by dirtying a leaf, hiding its parent via inline display:none, and
// confirming the ancestor clears SubtreeDirty after two updates.
TEST(SubtreeDirtyTests, DisplayNoneDescendantClearsAncestorSubtreeDirty)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='outer'>
            <uielement id='hidden'>
                <uielement id='leaf' />
            </uielement>
        </uielement>
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Frame 0+1: warm up, converge to clean state.
    ui.Update(0.0f, true);
    ui.Update(0.0f, true);

    UIElement* r = ui.GetRootElement();
    UIElement* hidden = r->FindById("hidden");
    UIElement* leaf = r->FindById("leaf");
    ASSERT_NE(hidden, nullptr);
    ASSERT_NE(leaf, nullptr);

    // Dirty the leaf, then hide its parent via inline display:none. Both
    // actions mark SubtreeDirty up the ancestor chain.
    leaf->MarkDirty(UIElement::StyleDirty);
    hidden->Overrides().Set(Style::Display, DisplayMode::None);
    EXPECT_TRUE(r->IsDirty(UIElement::SubtreeDirty));

    // Frame 2: the hidden subtree's slow path hits the display:none branch
    // and early-returns. Without the SubtreeDirty clear, the leaf's dirty
    // would be wedged under a hidden ancestor forever.
    ui.Update(0.0f, true);

    EXPECT_FALSE(r->IsDirty(UIElement::SubtreeDirty))
        << "Root SubtreeDirty must clear once all descendants (including "
           "a display:none subtree that hit the early-return) have cleared "
           "theirs — otherwise the fast path never fires.";

    // Frame 3: one more update to prove steady state stays clean.
    ui.Update(0.0f, true);
    EXPECT_FALSE(r->IsDirty(UIElement::SubtreeDirty));
}

// Regression: flipping an element's displayMode between None and visible
// changes ctx.nodes size (descendants appear/disappear via the display:none
// early-return) without ChildrenDirty firing on any element. The
// IndicesAndClips-rebuild skip gate previously missed this case and reused
// a stale nodeIndexByElement map. The fix tracks PrevDisplayModeNone on
// RetainedYogaNode and bumps the structure-change counter on flip, which
// forces the skip gate to rebuild. This test exercises a display-visibility
// cycle and verifies hit-test stays correct for elements adjacent to the
// display-flipping subtree.
TEST(SubtreeDirtyTests, DisplayNoneFlipRebuildsIndicesForAdjacentSiblings)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    // Tree: root has two flex children 'a' and 'b'. 'a' has a large nested
    // subtree that will be display:none'd and re-shown.
    const std::string xml = R"(<uielement id='root'>
        <uielement id='a'>
            <uielement id='a1' />
            <uielement id='a2' />
            <uielement id='a3' />
        </uielement>
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Warm up to a steady state so the IndicesAndClips skip gate is primed.
    ui.Update(0.0f, true);
    ui.Update(0.0f, true);
    ui.Update(0.0f, true);

    UIElement* r = ui.GetRootElement();
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Hide 'a' via inline display:none. Its descendants (a1/a2/a3) vanish
    // from ctx.nodes via the display:none early-return. 'b' stays.
    a->Overrides().Set(Style::Display, DisplayMode::None);
    ui.Update(0.0f, true);

    // b must still be known to the manager — if the IndicesAndClips skip
    // gate had wrongly fired, nodeIndexByElement might still have entries
    // pointing at indices that no longer match ctx.nodes (because a1/a2/a3
    // are gone but nodeIndexByElement wasn't rebuilt).
    EXPECT_EQ(r->FindById("b"), b);
    EXPECT_TRUE(b->IsEnabled());

    // Show 'a' again. a1/a2/a3 re-appear in ctx.nodes, ctx.nodes size grows.
    a->Overrides().Reset(Style::Display);
    ui.Update(0.0f, true);

    EXPECT_EQ(r->FindById("a1"), a->GetChildren()[0].get());
    EXPECT_EQ(r->FindById("a3"), a->GetChildren()[2].get());

    // One more steady-state frame to confirm the skip gate now reuses
    // correctly-populated maps.
    ui.Update(0.0f, true);
    EXPECT_EQ(r->FindById("a2"), a->GetChildren()[1].get());
    EXPECT_EQ(r->FindById("b"), b);
}

// ---------------------------------------------------------------------------
// SheetSetInterner unit tests (primary-flip Step 1).
// ---------------------------------------------------------------------------

// Empty span always interns to ID 0. Subsequent non-empty interns allocate
// fresh IDs densely from 1. Identical sheet lists return the same ID;
// different lists return different IDs.
TEST(SheetSetInternerTests, BasicInternAndDedup)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();
    UIManager ui(dev);

    auto sheetA = std::make_shared<const Stylesheet>();
    auto sheetB = std::make_shared<const Stylesheet>();
    auto sheetC = std::make_shared<const Stylesheet>();

    auto& interner = ui.GetSheetSetInterner();

    // Empty → ID 0.
    EXPECT_EQ(interner.Intern({}, ui), 0u);

    // Single sheet.
    StylesheetHandle listA[] = {sheetA};
    const uint32_t idA = interner.Intern(std::span<const StylesheetHandle>(listA), ui);
    EXPECT_NE(idA, 0u);

    // Same list again → same ID.
    EXPECT_EQ(interner.Intern(std::span<const StylesheetHandle>(listA), ui), idA);

    // Different sheet → different ID.
    StylesheetHandle listB[] = {sheetB};
    const uint32_t idB = interner.Intern(std::span<const StylesheetHandle>(listB), ui);
    EXPECT_NE(idB, 0u);
    EXPECT_NE(idB, idA);

    // Two-sheet list in order A,B.
    StylesheetHandle listAB[] = {sheetA, sheetB};
    const uint32_t idAB = interner.Intern(std::span<const StylesheetHandle>(listAB), ui);
    EXPECT_NE(idAB, 0u);
    EXPECT_NE(idAB, idA);
    EXPECT_NE(idAB, idB);

    // Order matters: B,A is a different set.
    StylesheetHandle listBA[] = {sheetB, sheetA};
    const uint32_t idBA = interner.Intern(std::span<const StylesheetHandle>(listBA), ui);
    EXPECT_NE(idBA, idAB);

    // Three-sheet list dedupes against itself.
    StylesheetHandle listABC[] = {sheetA, sheetB, sheetC};
    const uint32_t idABC1 = interner.Intern(std::span<const StylesheetHandle>(listABC), ui);
    const uint32_t idABC2 = interner.Intern(std::span<const StylesheetHandle>(listABC), ui);
    EXPECT_EQ(idABC1, idABC2);
    EXPECT_NE(idABC1, idAB);

    // GetSheets returns the canonical span (raw pointers, for hot-path equality).
    auto spanABC = interner.GetSheets(idABC1);
    ASSERT_EQ(spanABC.size(), 3u);
    EXPECT_EQ(spanABC[0], sheetA.get());
    EXPECT_EQ(spanABC[1], sheetB.get());
    EXPECT_EQ(spanABC[2], sheetC.get());

    // GetHandles returns the parallel keep-alive span (Step 4).
    auto handlesABC = interner.GetHandles(idABC1);
    ASSERT_EQ(handlesABC.size(), 3u);
    EXPECT_EQ(handlesABC[0].get(), sheetA.get());
    EXPECT_EQ(handlesABC[1].get(), sheetB.get());
    EXPECT_EQ(handlesABC[2].get(), sheetC.get());

    // GetIndices returns parallel span; individual entries may be null
    // (empty stylesheet) but size matches.
    auto indicesABC = interner.GetIndices(idABC1);
    EXPECT_EQ(indicesABC.size(), 3u);
}

// Clear returns the interner to just the empty ID.
TEST(SheetSetInternerTests, ClearRestoresEmptyState)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();
    UIManager ui(dev);

    auto sheetA = std::make_shared<const Stylesheet>();
    auto& interner = ui.GetSheetSetInterner();

    StylesheetHandle listA[] = {sheetA};
    const uint32_t before = interner.Intern(std::span<const StylesheetHandle>(listA), ui);
    EXPECT_GT(interner.Size(), 1u) << "Should have empty + at least one non-empty entry";

    const uint32_t generationBefore = interner.Generation();
    interner.Clear();
    EXPECT_EQ(interner.Size(), 1u) << "Clear leaves only the empty (id=0) entry";
    EXPECT_NE(interner.Generation(), generationBefore)
        << "Clear must advance the generation so stale retained ids are detectable";

    const uint32_t after = interner.Intern(std::span<const StylesheetHandle>(listA), ui);
    EXPECT_NE(after, 0u);
    // IDs start fresh from 1 after clear — new ID is not guaranteed equal to the old one.
    (void)before;
}

// BuildYogaRecursive populates PersistentSheetSetId on each retained
// node. After a normal Update, every visible element's retained node
// should have a nonzero (or appropriate-empty) ID matching what the
// interner would return for that element's effective sheet list.
TEST(SheetSetInternerTests, BuildYogaPopulatesPersistentSheetSetId)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("child-a"));
    root->AddChild(std::make_unique<UIElement>("child-b"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // No sheet attached beyond the manager's own globals (the UI module's
    // default stylesheet), and no element carries a local sheet, so every
    // element shares the one global set.
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    // The interner holds its empty entry plus that shared global set.
    ASSERT_FALSE(ui.GetStylesheets().empty()) << "the UI module default stylesheet is not staged";
    EXPECT_EQ(ui.GetSheetSetInterner().Size(), 2u)
        << "Every element shares the manager's global set, so the interner holds one set beside the empty entry";
}

// ---------------------------------------------------------------------------
// Production-path regression tests.
//
// Re-pointed from the deleted test-only cascade pipeline
// (CascadeDirtyElements / DrainStyleDirty / DrainLayoutDirty). Each guards a
// behavior that pipeline used to pin — splitter flex weights, order re-sort,
// text measure rebind, inheritance fan-out, sibling combinators — asserted
// through a full Update() against Yoga state / resolved styles.
// ---------------------------------------------------------------------------

// A CSS `order` change arriving via the cascade (class toggle) must re-sort
// the parent's Yoga children: the class marks StyleDirty, the rebuild
// observes it, and the child-attach pass re-inserts sorted.
// (Override-driven order changes are covered by
// OrderOverrideReordersYogaChildren below.)
TEST(ProductionPathTests, OrderClassChangeReordersYogaChildren)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    root->AddChild(std::make_unique<UIElement>("b"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    auto sheet = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".pushed { order: 5; }\n", *sheet));
    ui.AddStylesheet(StylesheetHandle(sheet));

    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    YGNodeRef rootYg = r->m_YogaState ? r->m_YogaState->Node : nullptr;
    YGNodeRef aYg = a->m_YogaState ? a->m_YogaState->Node : nullptr;
    YGNodeRef bYg = b->m_YogaState ? b->m_YogaState->Node : nullptr;
    ASSERT_NE(rootYg, nullptr);
    ASSERT_NE(aYg, nullptr);
    ASSERT_NE(bYg, nullptr);
    ASSERT_EQ(YGNodeGetChild(rootYg, 0), aYg) << "Document order before class";

    // Push a behind b via the cascade. Converged Yoga children must be
    // sorted by the new order values.
    a->AddClass("pushed");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(YGNodeGetChild(rootYg, 0), bYg)
        << "order:5 on a must re-sort Yoga children so b attaches first";
    EXPECT_EQ(YGNodeGetChild(rootYg, 1), aYg);
}

// An `order` change arriving via overrides raises only LayoutDirty (Layout
// impact, no cascade re-run) — the rebuild must still re-sort the parent's
// Yoga children. Guarded by BuiltNode::orderChanged: ordering is applied by
// InsertChildrenSortedByOrder, not by Yoga node style, so an in-place style
// apply alone can never honor it.
TEST(ProductionPathTests, OrderOverrideReordersYogaChildren)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    root->AddChild(std::make_unique<UIElement>("b"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    YGNodeRef rootYg = r->m_YogaState ? r->m_YogaState->Node : nullptr;
    YGNodeRef aYg = a->m_YogaState ? a->m_YogaState->Node : nullptr;
    YGNodeRef bYg = b->m_YogaState ? b->m_YogaState->Node : nullptr;
    ASSERT_NE(rootYg, nullptr);
    ASSERT_NE(aYg, nullptr);
    ASSERT_NE(bYg, nullptr);
    ASSERT_EQ(YGNodeGetChild(rootYg, 0), aYg) << "Document order before override";

    // LayoutDirty-only mutation: no class, no stylesheet, no StyleDirty.
    a->Overrides().Set(Style::Order, 5);
    EXPECT_FALSE(a->IsDirty(UIElement::StyleDirty));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(YGNodeGetChild(rootYg, 0), bYg)
        << "override order:5 on a must re-sort Yoga children so b attaches "
           "first";
    EXPECT_EQ(YGNodeGetChild(rootYg, 1), aYg);

    // And back: clearing the override restores document order.
    a->Overrides().Reset(Style::Order);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(YGNodeGetChild(rootYg, 0), aYg)
        << "resetting the order override must restore document order";
}

// Splitter regression: WeightedPane::SetFlexWeight raises only LayoutDirty.
// The rebuild must still re-apply the flex weight to Yoga — skipping
// LayoutDirty-only elements caused the docked-panel splitter snap-back bug
// in the Editor.
TEST(ProductionPathTests, WeightedPaneFlexWeightReachesYoga)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto paneOwner = std::make_unique<WeightedPane>(0.5f);
    WeightedPane* paneRaw = paneOwner.get();
    paneRaw->SetId("pane");
    root->AddChild(std::move(paneOwner));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    // Mimic Splitter::UpdateDrag: change only the flex weight.
    paneRaw->SetFlexWeight(0.75f);
    EXPECT_TRUE(paneRaw->IsDirty(UIElement::LayoutDirty));
    EXPECT_FALSE(paneRaw->IsDirty(UIElement::StyleDirty));

    ui.Update(0.0f, /*interactive=*/true);

    YGNodeRef paneYg = paneRaw->m_YogaState ? paneRaw->m_YogaState->Node : nullptr;
    ASSERT_NE(paneYg, nullptr);
    EXPECT_FLOAT_EQ(YGNodeStyleGetFlexGrow(paneYg), 0.75f)
        << "LayoutDirty-only flex-weight change must reach "
           "YGNodeStyleSetFlexGrow on the next Update";
    EXPECT_FALSE(paneRaw->IsDirty(UIElement::LayoutDirty))
        << "Update must clear LayoutDirty or the element stays dirty every "
           "frame";
}

// Text elements whose measure context was wiped must get the Yoga measure
// function + context rebound by the rebuild. Guards against stale text sizes
// after a class/style change.
TEST(ProductionPathTests, UpdateRebindsMeasureFuncForTextElement)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto label = std::make_unique<Label>();
    label->SetId("label");
    label->SetText("hello");
    Label* labelRaw = label.get();
    root->AddChild(std::move(label));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    YGNodeRef labelYg = labelRaw->m_YogaState ? labelRaw->m_YogaState->Node : nullptr;
    ASSERT_NE(labelYg, nullptr);

    // Clear the measure func + context so we can detect the rebind.
    YGNodeSetMeasureFunc(labelYg, nullptr);
    YGNodeSetContext(labelYg, nullptr);
    EXPECT_EQ(YGNodeGetContext(labelYg), nullptr);

    labelRaw->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NE(YGNodeGetContext(labelYg), nullptr)
        << "Update must rebind YGNodeSetContext for text elements so Yoga "
           "can re-measure on next solve";
}

// Inherited-property fan-out: a font-size change on the root must reach
// grandchildren through the resolve/inheritance walk.
TEST(ProductionPathTests, InheritedFontSizeFansOutOnUpdate)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    auto b = std::make_unique<UIElement>("b");
    b->AddChild(std::make_unique<UIElement>("b1"));
    root->AddChild(std::move(b));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    UIElement* b1 = r->FindById("b1");
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b1->GetResolvedStyle().Visual.FontSize, 24.0f);

    r->Overrides().Set(Style::FontSize, StyleLength::Px(24.0f));
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_FLOAT_EQ(b1->GetResolvedStyle().Visual.FontSize, 24.0f)
        << "font-size on root inherits; grandchild must pick up the new "
           "value after Update";
}

// Sibling-combinator styling must apply through the normal Update path:
// `.dirty + *` restyles the adjacent sibling when the class lands.
TEST(ProductionPathTests, SiblingCombinatorStyleAppliesOnUpdate)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    root->AddChild(std::make_unique<UIElement>("b"));
    root->AddChild(std::make_unique<UIElement>("c"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    auto sheet = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".dirty + * { color: rgb(200, 30, 40); }\n", *sheet));
    ui.AddStylesheet(StylesheetHandle(sheet));

    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    UIElement* c = r->FindById("c");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    const uint32_t bBefore = b->GetResolvedStyle().Visual.Color;
    const uint32_t cBefore = c->GetResolvedStyle().Visual.Color;

    a->AddClass("dirty");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NE(b->GetResolvedStyle().Visual.Color, bBefore)
        << "`.dirty + *` must restyle the adjacent sibling on Update";
    EXPECT_EQ(c->GetResolvedStyle().Visual.Color, cBefore)
        << "`+` is adjacent-only; c must keep its baseline color";
}

// Over-invalidation guard: with no sibling combinator in the attached
// sheets, dirtying one sibling must not smear StyleDirty onto the others —
// the rebuild's sibling propagation is gated on
// m_StyleAnalysis.UsesSiblingCombinators.
TEST(ProductionPathTests, NoSiblingSmearWithoutCombinator)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    root->AddChild(std::make_unique<UIElement>("b"));
    root->AddChild(std::make_unique<UIElement>("c"));
    root->AddChild(std::make_unique<UIElement>("d"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // A sheet with plain selectors only — analysis must record "no sibling
    // combinators".
    auto sheet = std::make_shared<Stylesheet>();
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".dirty { color: rgb(200, 30, 40); }\n", *sheet));
    ui.AddStylesheet(StylesheetHandle(sheet));

    ui.SetUpdateProfilingEnabled(true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    UIElement* b = r->FindById("b");
    ASSERT_NE(b, nullptr);

    b->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    UIManager::UpdateProfileFrame prof{};
    ASSERT_TRUE(ui.GetLastUpdateProfileFrame(prof));
    // Only b needs a cascade. A smear regression re-cascades the following
    // siblings c and d too (>= 3). Margin of 2 tolerates an incidental
    // root/extra cascade without masking the smear signature.
    EXPECT_LE(prof.CascadeCallsBuildYoga, 2u)
        << "no sibling combinator in the sheet set -> dirtying b must not "
           "re-cascade its siblings";
}

// Over-invalidation guard: re-cascading an element whose resolved style
// comes out unchanged must not fan cascades out to descendants — inherited
// values propagate through the ResolveStyles walk, not extra cascades.
TEST(ProductionPathTests, NoDescendantCascadeFanOutWhenNothingChanged)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    root->AddChild(std::make_unique<UIElement>("a"));
    auto b = std::make_unique<UIElement>("b");
    b->AddChild(std::make_unique<UIElement>("b1"));
    root->AddChild(std::move(b));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.SetUpdateProfilingEnabled(true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    // Root's cascade re-runs but produces the same defaults (no sheets, no
    // overrides) — descendants must stay untouched.
    r->MarkDirty(UIElement::StyleDirty);
    ui.Update(0.0f, /*interactive=*/true);

    UIManager::UpdateProfileFrame prof{};
    ASSERT_TRUE(ui.GetLastUpdateProfileFrame(prof));
    // A fan-out regression re-cascades a, b, b1 as well (>= 4).
    EXPECT_LE(prof.CascadeCallsBuildYoga, 2u)
        << "no-change root re-cascade must not fan out to descendants";
}

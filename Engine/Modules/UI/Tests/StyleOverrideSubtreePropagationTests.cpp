// GitHub #793 — a bare `Overrides().Set(...)` marks the element it was called
// on, and nothing else. For a property whose effect stops at that element's own
// paint box that is exactly right. For one whose effect reaches DESCENDANTS it
// is not, and the descendants keep rendering the pre-change value.
//
// THE CHAIN, and where it breaks:
//
//   StyleOverrides::SetImpl raises VisualDirty for a paint-impact property
//     -> UIElement::MarkDirty(VisualDirty)
//     -> UIManager::NotifyDirty_UpdateRegenFlag: VisualDirty is not in
//        kLayoutAffecting, so no full regen is requested
//     -> the E1 drain (DrainPrimitiveDataDirty) rewrites the marked element's
//        own primitive bytes, with no DFS and no child recursion.
//
// Nothing in that chain visits a child. `opacity` is the specimen because the
// emit walk multiplies it into every descendant primitive
// (UIManager_PrimitiveGen.cpp: `ctx.EffectiveOpacity *= style.Visual.Opacity`,
// baked in by SetOpacity), so a parent's change genuinely alters what the child
// paints -- and the child is never re-emitted.
//
// NO EXPLICIT MarkDirty ANYWHERE IN THE SPECIMENS, and that is the whole point.
// ZeroRectDirtyGateTests.cpp next door mutates through the same override API but
// always follows with MarkDirty(StyleDirty | VisualDirty), which reaches
// kLayoutAffecting and forces the full regen that hides this defect. These tests
// exercise the path the editor's own mutate-only call sites take.
//
// ONE ARM PER FIXTURE, for the reason ZeroRectDirtyGateTests states:
// m_PrimitivesNeedRegen is a single global flag, so an arm that legitimately
// requests a regen refreshes every other arm's subtree in the same frame and
// hides the defect completely.
//
// SCOPE — three axes are asserted, not one, because "the effect reaches
// descendants" has more than one mechanism behind it and a fix aimed at only
// the first invites the next one:
//
//   opacity    the emit walk MULTIPLIES it down the subtree.
//   visibility CSS INHERITANCE carries it down; a descendant that does not
//              state its own resolves to the parent's.
//   overflow   it establishes the CLIP descendants are emitted against.
//
// Only the first is named in #793. The other two are here because they share
// the mark, not because the issue claimed them.

#include "UIRgTestHarness.h"

#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

// A normally-sized parent with one sized, painting child. The child owns a
// primitive whose baked opacity, clip index and existence can all be read back,
// which is what makes each axis observable from outside the element that was
// marked.
struct ParentChildFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    UIElement* Parent = nullptr;
    UIElement* Child = nullptr;

    bool Init(const char* cssFileName, const char* childCss)
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto parent = std::make_unique<UIElement>();
        Parent = parent.get();
        parent->SetId("parent");
        auto child = std::make_unique<UIElement>();
        Child = child.get();
        child->SetId("child");
        parent->AddChild(std::move(child));
        root->AddChild(std::move(parent));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / cssFileName;
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 220px; height: 240px; }\n"
              << "#parent { display: flex; flex-direction: column; width: 100px; height: 40px; "
                 "overflow: visible; flex-shrink: 0; }\n"
              << "#child { " << childCss << " }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 3; ++i)
            Pump();
        return true;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    const UI::UIPrimitive* ChildPrim() const { return Ui->PeekPrimitiveForTesting(*Child, 0); }

    float ChildOpacityOr(float fallback) const
    {
        const UI::UIPrimitive* p = ChildPrim();
        return p ? p->Opacity : fallback;
    }

    ~ParentChildFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

constexpr float kFadedOpacity = 0.35f;

// Fits inside the parent, so it is never clipped and its primitive is present
// in every arm that is not about clipping.
constexpr char kContainedChild[] =
    "width: 80px; height: 24px; background-color: #808080; flex-shrink: 0;";

// Wider than the parent, so an overflow clip on the parent is observable on the
// child's primitive as a clip index.
constexpr char kOverflowingChild[] =
    "width: 200px; height: 24px; background-color: #808080; flex-shrink: 0;";

} // namespace

// ---------------------------------------------------------------------------
// opacity — the specimen #793 names
// ---------------------------------------------------------------------------

// The control, in its own fixture. The same mutation, followed by the explicit
// mark the editor's mutate-then-mark call sites use. This is what makes the
// specimen's failure attributable to the mark that SetImpl chose and not to
// opacity failing to reach descendant primitives at all.
TEST(StyleOverrideSubtreePropagation, ControlAnExplicitStyleMarkReachesTheChild)
{
    ParentChildFixture fx;
    if (!fx.Init("ui_override_subtree_opacity_control.css", kContainedChild))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f)
        << "child must emit, and start at an opacity other than the one under test";

    fx.Parent->Overrides().Set(Style::Opacity, kFadedOpacity);
    fx.Parent->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    fx.Pump();

    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "with a layout-affecting mark the full regen runs and the child follows";
}

// The specimen. Identical to the control except that nothing marks the element
// beyond what SetImpl itself raises.
TEST(StyleOverrideSubtreePropagation, OpacitySetThroughOverridesAloneReachesTheChild)
{
    ParentChildFixture fx;
    if (!fx.Init("ui_override_subtree_opacity.css", kContainedChild))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f);

    fx.Parent->Overrides().Set(Style::Opacity, kFadedOpacity);
    fx.Pump();

    // Instrument: separate "the override never reached the cascade" from "the
    // subtree was never re-emitted". Only the second is #793.
    ASSERT_NEAR(fx.Parent->GetResolvedStyle().Visual.Opacity, kFadedOpacity, 0.001f)
        << "instrument: the override must reach the parent's ResolvedStyle before its effect "
           "on the child can be read";

    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "a parent's opacity is multiplied into every descendant primitive; setting it "
           "through Overrides alone must not leave them holding the old product";
}

// A mark that is never raised is not recovered by waiting. Without this, a fix
// that merely deferred the refresh by a frame would look correct.
TEST(StyleOverrideSubtreePropagation, TheStaleChildDoesNotHealOnLaterFrames)
{
    ParentChildFixture fx;
    if (!fx.Init("ui_override_subtree_opacity_heal.css", kContainedChild))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f);

    fx.Parent->Overrides().Set(Style::Opacity, kFadedOpacity);
    for (int i = 0; i < 4; ++i)
        fx.Pump();

    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "four idle frames must not leave the subtree stale";
}

// ---------------------------------------------------------------------------
// The other two ways a paint-only property reaches descendants
// ---------------------------------------------------------------------------

// visibility is INHERITED, so a descendant that does not state its own resolves
// to the parent's. Hiding the parent must stop the child painting.
TEST(StyleOverrideSubtreePropagation, VisibilitySetThroughOverridesAloneReachesTheChild)
{
    ParentChildFixture fx;
    if (!fx.Init("ui_override_subtree_visibility.css", kContainedChild))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NE(fx.ChildPrim(), nullptr) << "the child must paint before hiding can be observed";

    fx.Parent->Overrides().Set(Style::Visibility, false);
    fx.Pump();

    ASSERT_FALSE(fx.Parent->GetResolvedStyle().Visual.Visible)
        << "instrument: the override must reach the parent's ResolvedStyle first";

    EXPECT_EQ(fx.ChildPrim(), nullptr)
        << "visibility is inherited; hiding a parent through Overrides alone must not leave "
           "its descendants painting";
}

// overflow establishes the clip descendants are emitted against. Turning it on
// must clip the child that hangs out of the parent.
TEST(StyleOverrideSubtreePropagation, OverflowSetThroughOverridesAloneClipsTheChild)
{
    ParentChildFixture fx;
    if (!fx.Init("ui_override_subtree_overflow.css", kOverflowingChild))
        GTEST_SKIP() << "no headless GPU device";

    const UI::UIPrimitive* before = fx.ChildPrim();
    ASSERT_NE(before, nullptr) << "the child must paint before its clip can be observed";
    ASSERT_EQ(UI::GetClipIndex(before->ModeAndFlags), UI::kNoClip)
        << "instrument: an overflow:visible parent must leave the child unclipped, or this "
           "test cannot tell a clip from the initial state";

    fx.Parent->Overrides().Set(Style::OverflowProp, Overflow::Hidden);
    fx.Pump();

    ASSERT_EQ(fx.Parent->GetResolvedStyle().Layout.Overflow, Overflow::Hidden)
        << "instrument: the override must reach the parent's ResolvedStyle first";

    const UI::UIPrimitive* after = fx.ChildPrim();
    ASSERT_NE(after, nullptr) << "clipping must not delete the child's primitive";
    EXPECT_NE(UI::GetClipIndex(after->ModeAndFlags), UI::kNoClip)
        << "overflow establishes the clip descendants are emitted against; setting it through "
           "Overrides alone must not leave them unclipped";
}

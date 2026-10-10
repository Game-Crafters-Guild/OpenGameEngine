// GitHub #777, fourth walk — the dirty gate still believed a zero-rect element
// produces nothing.
//
// NotifyDirty_UpdateRegenFlag (UIManager.cpp) drops every layout-affecting mark
// on an element whose layout rect is empty, on the premise that such an element
// emits no primitives. #777 made that premise false for the SUBTREE: a 0x0 box
// with `overflow: visible` no longer stops the emit walk, so its children are in
// the snapshot and the snapshot can go stale behind a mark thrown away here.
//
// THE ASSERTION HAS TO BE ABOUT THE CHILD, for the same reason it did in
// ZeroSizeSubtreeTests: the zero-rect element emits nothing before or after the
// fix, so its own primitives look identical either way. What changes is whether
// the CHILD's already-emitted primitives get refreshed.
//
// SPECIMEN NOTE — `opacity` is the mutation, and both halves of that choice are
// load-bearing:
//
//   * opacity is a VISUAL property, so re-resolving it moves no Yoga input.
//     anyLayoutSignatureChanged stays false, so needInitialLayoutSolve
//     (UIManager_Update.cpp) stays false, no solve runs, and CommitLayoutRects —
//     which is what actually escalates a rect change to a full regen — never
//     executes. This is what defeats the fallback the gate's comment cited.
//   * opacity is nonetheless MULTIPLICATIVE DOWN THE SUBTREE
//     (UIManager_PrimitiveGen.cpp: `ctx.EffectiveOpacity *= style.Visual.Opacity`,
//     baked into every descendant primitive by SetOpacity), so the child's
//     primitives really are stale rather than merely unrefreshed.
//
// ONE ARM PER FIXTURE, and that is not stylistic. m_PrimitivesNeedRegen is a
// single global flag: marking the sized control in the same frame as the
// specimen triggers a full DFS that refreshes the specimen's subtree too and
// hides the defect completely. The control therefore runs in its own fixture.
//
// MARKING NOTE — StyleOverrides::SetImpl raises VisualDirty ONLY for a
// paint-impact property, which never reaches the layout-affecting branch under
// test. The mark is therefore explicit, matching the editor's own mutate-then-
// mark idiom (EditorTopToolbar.cpp:515 and ~14 peer call sites).

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

// One parent, one child with its own size and background so the child owns a
// primitive whose baked opacity can be read back. `boxCss` decides which arm
// this fixture is.
struct OneBoxFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    UIElement* Box = nullptr;
    UIElement* Child = nullptr;

    bool Init(const std::string& boxCss, const char* cssFileName)
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto box = std::make_unique<UIElement>();
        Box = box.get();
        box->SetId("box");
        auto child = std::make_unique<UIElement>();
        Child = child.get();
        child->SetId("child");
        box->AddChild(std::move(child));
        root->AddChild(std::move(box));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));
        Ui->SetUpdateProfilingEnabled(true);

        const auto css = std::filesystem::temp_directory_path() / cssFileName;
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 220px; height: 240px; }\n"
              << "#box { " << boxCss << " }\n"
              << "#child { width: 100px; height: 24px; background-color: #808080; "
                 "flex-shrink: 0; }\n";
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

    float ChildOpacityOr(float fallback) const
    {
        const UI::UIPrimitive* p = Ui->PeekPrimitiveForTesting(*Child, 0);
        return p ? p->Opacity : fallback;
    }

    ~OneBoxFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

constexpr float kFadedOpacity = 0.35f;

// Zero HEIGHT, children not clipped away, so the emit walk descends
// (EmitWalkEntersSubtree) and the child is genuinely in the snapshot.
constexpr char kZeroRectBox[] =
    "display: flex; flex-direction: column; width: 100px; height: 0px; "
    "overflow: visible; flex-shrink: 0;";
// Identical but with a real height.
constexpr char kSizedBox[] =
    "display: flex; flex-direction: column; width: 100px; height: 24px; "
    "overflow: visible; flex-shrink: 0;";

} // namespace

// The control, in its own fixture. A sized parent's rect passes the gate, so
// the mark sets the regen flag and the child refreshes. This is what makes the
// specimen's failure attributable to the empty rect and not to opacity failing
// to reach descendant primitives at all.
TEST(ZeroRectDirtyGate, ControlSizedParentPropagatesOpacityToItsChild)
{
    OneBoxFixture fx;
    if (!fx.Init(kSizedBox, "ui_dirty_gate_sized.css"))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f)
        << "child must emit, and start at an opacity other than the one under test";

    fx.Box->Overrides().Set(Style::Opacity, kFadedOpacity);
    fx.Box->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    fx.Pump();

    ASSERT_NEAR(fx.Box->GetResolvedStyle().Visual.Opacity, kFadedOpacity, 0.001f)
        << "instrument: the override must reach ResolvedStyle before its effect can be read";
    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "a sized parent's opacity change must reach its child's primitive";
}

// The specimen.
TEST(ZeroRectDirtyGate, ZeroRectParentPropagatesOpacityToItsChild)
{
    OneBoxFixture fx;
    if (!fx.Init(kZeroRectBox, "ui_dirty_gate_zero.css"))
        GTEST_SKIP() << "no headless GPU device";

    // Instrument check: the premise this rests on is that a zero-height
    // parent's child emits at all. That is what #777 bought; without it the
    // assertion below would be vacuous rather than failing.
    ASSERT_NE(fx.Ui->PeekPrimitiveForTesting(*fx.Child, 0), nullptr)
        << "a zero-height parent's child must emit before its refresh can be tested";
    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f);

    fx.Box->Overrides().Set(Style::Opacity, kFadedOpacity);
    fx.Box->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    fx.Pump();

    // Instrument check: separate "the style never changed" from "the snapshot
    // was never refreshed". Only the second is the defect.
    ASSERT_NEAR(fx.Box->GetResolvedStyle().Visual.Opacity, kFadedOpacity, 0.001f)
        << "instrument: the override must reach ResolvedStyle before its effect can be read";

    // NotifyDirty_UpdateRegenFlag drops the mark on the empty rect, no solve
    // runs to escalate it, and the child keeps rendering at the pre-change
    // opacity until something unrelated forces a full regen.
    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "a zero-rect parent must not swallow a mark that changes what its subtree paints";
}

// A dropped mark is not self-healing: extra frames do not recover it. Without
// this, a fix that merely deferred the refresh by a frame would look correct.
TEST(ZeroRectDirtyGate, ZeroRectStalenessDoesNotHealOnLaterFrames)
{
    OneBoxFixture fx;
    if (!fx.Init(kZeroRectBox, "ui_dirty_gate_zero_heal.css"))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_NEAR(fx.ChildOpacityOr(-1.0f), 1.0f, 0.001f);
    fx.Box->Overrides().Set(Style::Opacity, kFadedOpacity);
    fx.Box->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    for (int i = 0; i < 4; ++i)
        fx.Pump();

    EXPECT_NEAR(fx.ChildOpacityOr(-1.0f), kFadedOpacity, 0.001f)
        << "four idle frames must not leave the subtree stale";
}

// --- Guards on the fix's other edge ---------------------------------------
//
// Routing the gate through EmitWalkEntersSubtree does not only widen it. That
// predicate also answers false for display:none and opacity<=0, which the rect
// test alone did not, so the gate gets STRICTER for elements that have a real
// rect but are hidden by style. These two pin that the marks which un-hide such
// an element still reach a regen. They pass before the fix as well — they exist
// to catch the fix over-reaching, not to demonstrate the defect.

TEST(ZeroRectDirtyGate, GuardOpacityZeroParentBecomingVisibleStillRepaints)
{
    OneBoxFixture fx;
    if (!fx.Init("display: flex; flex-direction: column; width: 100px; height: 24px; "
                 "overflow: visible; flex-shrink: 0; opacity: 0;",
                 "ui_dirty_gate_guard_opacity.css"))
        GTEST_SKIP() << "no headless GPU device";

    // A fully transparent parent culls its subtree from the DFS, so the child
    // owns no primitive to begin with.
    ASSERT_EQ(fx.Ui->PeekPrimitiveForTesting(*fx.Child, 0), nullptr)
        << "instrument: an opacity:0 parent must cull its subtree, or this guard proves nothing";

    fx.Box->Overrides().Set(Style::Opacity, 1.0f);
    fx.Box->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    fx.Pump();

    ASSERT_NEAR(fx.Box->GetResolvedStyle().Visual.Opacity, 1.0f, 0.001f);
    const UI::UIPrimitive* p = fx.Ui->PeekPrimitiveForTesting(*fx.Child, 0);
    ASSERT_NE(p, nullptr) << "un-hiding an opacity:0 parent must bring its subtree back";
    EXPECT_NEAR(p->Opacity, 1.0f, 0.001f);
}

TEST(ZeroRectDirtyGate, GuardDisplayNoneParentBecomingVisibleStillRepaints)
{
    OneBoxFixture fx;
    if (!fx.Init("display: none; flex-direction: column; width: 100px; height: 24px; "
                 "overflow: visible; flex-shrink: 0;",
                 "ui_dirty_gate_guard_display.css"))
        GTEST_SKIP() << "no headless GPU device";

    ASSERT_EQ(fx.Ui->PeekPrimitiveForTesting(*fx.Child, 0), nullptr)
        << "instrument: a display:none parent must cull its subtree, or this guard proves nothing";

    fx.Box->Overrides().Set(Style::Display, DisplayMode::Flex);
    fx.Box->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    fx.Pump();

    ASSERT_NE(fx.Ui->PeekPrimitiveForTesting(*fx.Child, 0), nullptr)
        << "un-hiding a display:none parent must bring its subtree back";
}

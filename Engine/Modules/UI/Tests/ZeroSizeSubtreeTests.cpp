// GitHub #777 — a zero-size element silently drops its entire subtree.
//
// GeneratePrimitivesForElement returned outright on `w <= 0 || h <= 0`, before
// the visit stamp, before self-emission, before the clip push, before the
// overlay deferral and before child recursion. A 0x0 box painting no background
// of its own is correct; refusing to descend into children that have their own
// geometry is not — an overflowing or absolutely-positioned child of a 0x0
// parent renders in every browser.
//
// THE ASSERTION HAS TO BE ABOUT THE CHILD. This defect is structurally
// invisible to a test that inspects the zero-size element's own primitives:
// nothing was ever emitted for it, and nothing is *supposed* to be, so that
// element looks identical before and after the fix. What changes is whether the
// CHILD's primitives exist at all. Every test below therefore asserts on the
// descendant, and the parent's own emptiness is pinned separately so a fix that
// over-corrected — painting a 0x0 background rect — is caught too.
//
// SPECIMEN NOTE — `overflow` is left at its initial `visible` on the zero-size
// parent, and that is load-bearing. A 0x0 box with `overflow: hidden` clips its
// children to nothing, so skipping the whole subtree is the RIGHT answer there,
// and the engine still does (the clip cull covers it). A specimen that used the
// engine's more common collapsed-panel idiom would therefore assert the
// opposite of what it looks like it asserts. ZeroAreaClippingParentStillSkips-
// ItsSubtree pins that second branch so the two cannot be confused.

#include "IsolatedUIFixture.h"

#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIPrimitive.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
// The drain fixture at the bottom builds a live manager, so it needs the
// engine namespace the same way the sibling drain tests do.
using namespace GameEngine;

namespace
{

// Parents that differ only in how they reach zero area, each with one child
// that has a real size and a real background. `collapsedClipped` is the
// control: same zero area, but it clips its children. The `...Shadow` pair and
// the `clipper` subtree separate the two cull questions (see below).
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="zeroWidth"><uielement id="zeroWidthChild"/></uielement>
  <uielement id="zeroHeight"><uielement id="zeroHeightChild"/></uielement>
  <uielement id="zeroBoth"><uielement id="zeroBothChild"/></uielement>
  <uielement id="collapsedClipped"><uielement id="collapsedClippedChild"/></uielement>
  <uielement id="collapsedClippedShadow"><uielement id="collapsedClippedShadowChild"/></uielement>
  <uielement id="collapsedVisibleShadow"><uielement id="collapsedVisibleShadowChild"/></uielement>
  <uielement id="clipper">
    <uielement id="outsidePlain"><uielement id="outsidePlainChild"/></uielement>
    <uielement id="outsideShadow"><uielement id="outsideShadowChild"/></uielement>
  </uielement>
  <uielement id="sizedSibling"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }

/* The parents carry a background of their own on purpose: without one,
   "emits nothing itself" would be true whatever the code did. `sizedSibling`
   declares the identical background at a real size and is the control that
   makes the empty results below attributable to the zero area. */
#zeroWidth  { width: 0px;   height: 40px; overflow: visible; background-color: #0000ff; }
#zeroHeight { width: 100px; height: 0px;  overflow: visible; background-color: #0000ff; }
#zeroBoth   { width: 0px;   height: 0px;  overflow: visible; background-color: #0000ff; }
#collapsedClipped { width: 0px; height: 0px; overflow: hidden; background-color: #0000ff; }
#sizedSibling { width: 40px; height: 40px; overflow: visible; background-color: #0000ff; }

/* The two questions, separated. Both boxes are 0x0 and both carry a shadow;
   they differ only in whether they clip their children. A predicate that reads
   the shadow when deciding a ZERO-AREA descent answers the same for both. */
#collapsedClippedShadow {
  width: 0px; height: 0px; overflow: hidden; background-color: #0000ff;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.8);
}
#collapsedVisibleShadow {
  width: 0px; height: 0px; overflow: visible; background-color: #0000ff;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.8);
}

/* The other half: boxes WITH area whose rect misses the ambient clip. There the
   shadow is really painted and really spills, so it must still suppress the
   cull. `outsidePlain` is the same specimen without one. */
#clipper { width: 100px; height: 60px; overflow: hidden; flex-shrink: 0; }
#outsidePlain, #outsideShadow {
  position: absolute; left: 300px; top: 0px;
  width: 40px; height: 40px; overflow: hidden;
}
#outsideShadow { box-shadow: 0 4px 12px rgba(0, 0, 0, 0.8); }

/* Explicit on BOTH axes and non-shrinking, so the child keeps its size
   whichever axis the parent collapsed and whichever way the flow runs. */
#zeroWidthChild, #zeroHeightChild, #zeroBothChild, #collapsedClippedChild,
#collapsedClippedShadowChild, #collapsedVisibleShadowChild {
  flex-shrink: 0;
  width: 80px;
  height: 24px;
  background-color: #ff0000;
}

#outsidePlainChild, #outsideShadowChild {
  flex-shrink: 0;
  width: 20px;
  height: 20px;
  background-color: #ff0000;
}
)";

// The authored backgrounds as the primitives carry them: packed RGBA8, R in the
// low byte (UIPrimitive.h). Red for the children, blue for the parents.
constexpr uint32_t kChildFill = 0xFF0000FFu;
constexpr uint32_t kParentFill = 0xFFFF0000u;

bool HasRectWithFill(const std::vector<UIPrimitive>& prims, uint32_t fill)
{
    for (const UIPrimitive& p : prims)
    {
        if (GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect &&
            p.FillColor == fill)
            return true;
    }
    return false;
}

} // namespace

// A zero-WIDTH parent. The child has 80x24 of its own and must be emitted.
TEST(ZeroSizeSubtree, ZeroWidthParentStillEmitsItsChild)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument check: the premise is that the parent really is zero-width and
    // the child really is not. A CSS change that gave either one a different
    // size would make the emission assertion meaningless.
    const PhysicalRect parent = fx.BorderBox("zeroWidth");
    const PhysicalRect child = fx.BorderBox("zeroWidthChild");
    ASSERT_FLOAT_EQ(parent.W, 0.0f);
    ASSERT_GT(child.W, 0.0f);
    ASSERT_GT(child.H, 0.0f);

    EXPECT_TRUE(HasRectWithFill(fx.Primitives("zeroWidthChild"), kChildFill))
        << "a zero-width parent must not swallow a child that has its own geometry";
}

TEST(ZeroSizeSubtree, ZeroHeightParentStillEmitsItsChild)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect parent = fx.BorderBox("zeroHeight");
    const PhysicalRect child = fx.BorderBox("zeroHeightChild");
    ASSERT_FLOAT_EQ(parent.H, 0.0f);
    ASSERT_GT(child.W, 0.0f);
    ASSERT_GT(child.H, 0.0f);

    EXPECT_TRUE(HasRectWithFill(fx.Primitives("zeroHeightChild"), kChildFill))
        << "a zero-height parent must not swallow a child that has its own geometry";
}

TEST(ZeroSizeSubtree, ZeroAreaParentStillEmitsItsChild)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect parent = fx.BorderBox("zeroBoth");
    ASSERT_FLOAT_EQ(parent.W, 0.0f);
    ASSERT_FLOAT_EQ(parent.H, 0.0f);
    ASSERT_GT(fx.BorderBox("zeroBothChild").W, 0.0f);

    EXPECT_TRUE(HasRectWithFill(fx.Primitives("zeroBothChild"), kChildFill))
        << "a 0x0 parent must not swallow a child that has its own geometry";
}

// The other half of the contract: the zero-area box still paints nothing of its
// own. Restoring the descent must not restore self-emission — a background on a
// box with no area has nowhere to go. Each parent here authors one, so this
// says something the CSS could otherwise satisfy on its own.
TEST(ZeroSizeSubtree, ZeroAreaParentEmitsNothingItself)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument check: a sibling declaring the same background at a real size
    // does emit, so an empty result below is the zero area and nothing else.
    ASSERT_TRUE(HasRectWithFill(fx.Primitives("sizedSibling"), kParentFill));

    EXPECT_TRUE(fx.Primitives("zeroBoth").empty());
    EXPECT_TRUE(fx.Primitives("zeroWidth").empty());
    EXPECT_TRUE(fx.Primitives("zeroHeight").empty());
}

// The control. A 0x0 box with `overflow: hidden` clips its children to an empty
// rect, so nothing below it can be visible and the whole subtree is still
// skipped. This is what keeps a collapsed panel from costing a full descent, and
// it is why the specimens above have to spell out `overflow: visible`.
TEST(ZeroSizeSubtree, ZeroAreaClippingParentStillSkipsItsSubtree)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect parent = fx.BorderBox("collapsedClipped");
    ASSERT_FLOAT_EQ(parent.W, 0.0f);
    ASSERT_FLOAT_EQ(parent.H, 0.0f);
    ASSERT_GT(fx.BorderBox("collapsedClippedChild").W, 0.0f);

    EXPECT_FALSE(HasRectWithFill(fx.Primitives("collapsedClippedChild"), kChildFill))
        << "a 0x0 box that clips its children hides them, so the descent is dead work";
}

// --- Two questions that are not the same question -------------------------
//
// "Does this box clip its children to its own rect" and "is everything this box
// contributes bounded by its own rect" differ by the box's own spill — a shadow
// or a glow. For the CLIP-MISS cull the difference is load-bearing: the element
// paints, and the shadow it paints lands outside the rect the miss was measured
// against, so it must not be culled. For the ZERO-AREA descent the difference is
// noise: the box paints nothing at all, shadow included, so a spill that cannot
// happen cannot decide whether the subtree below is reachable.
//
// The pair below separates them. Both specimens are 0x0 with the same shadow and
// differ only in `overflow`, so a predicate that consults the shadow gives the
// same answer for both, and the one that matters is wrong.

TEST(ZeroSizeSubtree, ZeroAreaClippingParentWithAShadowStillSkipsItsSubtree)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect parent = fx.BorderBox("collapsedClippedShadow");
    ASSERT_FLOAT_EQ(parent.W, 0.0f);
    ASSERT_FLOAT_EQ(parent.H, 0.0f);
    ASSERT_GT(fx.BorderBox("collapsedClippedShadowChild").W, 0.0f);

    // Instrument check: the shadow has to have reached the cascade, or this
    // specimen is a second copy of the plain collapsed box and proves nothing.
    const auto* style = fx.Style("collapsedClippedShadow");
    ASSERT_NE(style, nullptr);
    ASSERT_GT(style->Visual.ShadowSoftness, 0.0f)
        << "box-shadow never reached the visual style — specimen is not a shadow specimen";

    EXPECT_FALSE(HasRectWithFill(fx.Primitives("collapsedClippedShadowChild"), kChildFill))
        << "a 0x0 box paints no shadow, so its shadow cannot make a clipped subtree visible";
}

// The guard against reading the pair above as "a shadow culls things". Same 0x0
// box, same shadow, `overflow: visible` — nothing clips the child away and it
// must still be emitted.
TEST(ZeroSizeSubtree, ZeroAreaNonClippingParentWithAShadowStillEmitsItsChild)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect parent = fx.BorderBox("collapsedVisibleShadow");
    ASSERT_FLOAT_EQ(parent.W, 0.0f);
    ASSERT_FLOAT_EQ(parent.H, 0.0f);
    ASSERT_GT(fx.BorderBox("collapsedVisibleShadowChild").W, 0.0f);

    EXPECT_TRUE(HasRectWithFill(fx.Primitives("collapsedVisibleShadowChild"), kChildFill))
        << "a 0x0 box that does not clip must still descend, shadow or no shadow";
}

// The other half of the split: with AREA, the shadow is painted and does spill,
// so it still has to suppress the clip-miss cull. `outsidePlain` is the control
// — identical specimen, no shadow — and it must stay culled. Moving the spill
// terms out of the zero-area question must not delete them from this one.
TEST(ZeroSizeSubtree, ShadowedBoxOutsideTheAmbientClipIsStillNotCulled)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument check: both specimens must really have area and really sit
    // outside the clipper, or "not culled" says nothing about the cull.
    const PhysicalRect clipper = fx.BorderBox("clipper");
    const PhysicalRect shadowed = fx.BorderBox("outsideShadow");
    const PhysicalRect plain = fx.BorderBox("outsidePlain");
    ASSERT_GT(shadowed.W, 0.0f);
    ASSERT_GT(shadowed.H, 0.0f);
    ASSERT_GE(shadowed.X, clipper.X + clipper.W) << "specimen must miss the ambient clip";
    ASSERT_GE(plain.X, clipper.X + clipper.W) << "control must miss the ambient clip";
    const auto* shadowStyle = fx.Style("outsideShadow");
    const auto* plainStyle = fx.Style("outsidePlain");
    ASSERT_NE(shadowStyle, nullptr);
    ASSERT_NE(plainStyle, nullptr);
    ASSERT_GT(shadowStyle->Visual.ShadowSoftness, 0.0f) << "specimen must carry a shadow";
    ASSERT_FLOAT_EQ(plainStyle->Visual.ShadowSoftness, 0.0f) << "control must not";

    EXPECT_FALSE(HasRectWithFill(fx.Primitives("outsidePlainChild"), kChildFill))
        << "control: a bounded box outside the clip takes its subtree with it";
    EXPECT_TRUE(HasRectWithFill(fx.Primitives("outsideShadowChild"), kChildFill))
        << "a shadow spills past the rect the miss was measured against, so the "
           "clip-miss cull must not fire";
}

// --- The drain path -------------------------------------------------------
//
// The full DFS is only one of the walks that has to know which subtrees the
// emit path enters. Two more restate the same gate so they can decide which
// elements to queue for the sparse primitive drain (TranslateLayoutSubtree and
// MarkSubtreeForDrainReEmit, UIManager_Layout.cpp). Fixing the DFS alone leaves
// a zero-size element's descendants emitted once and then frozen: their baked
// primitives never follow the element when a scroll translates its rect,
// because the walk that queues the re-emit stops at the zero-size ancestor.
//
// A headless layout change cannot reach this frame type — a class or `top`
// change re-solves Yoga and full-regens, which re-emits everything and hides
// the divergence. Scroll translation is the one path that moves committed
// rects on a pure drain frame, so the fixture below is a ScrollView.
//
// COVERAGE, STATED HONESTLY. The test below covers TranslateLayoutSubtree only.
// Reverting that walk's gate on its own fails it; reverting the OTHER walk's
// gate (MarkSubtreeForDrainReEmit) on its own leaves the whole suite green, so
// that site is fixed by shared predicate and by inspection, not by a test.
//
// It is not for want of trying. MarkSubtreeForDrainReEmit is reached only from
// the SIZE-change branch of ApplyLayoutOverrideRects, and it opens by returning
// early whenever a full regen is already pending. A headless size change made
// through UI::Layout::SetAbsolutePosition sets exactly that: measured on a
// move-and-resize patch frame built for this purpose, DrainItems was 0 and the
// child's primitive tracked its rect identically with the gate fixed and with
// it reverted. A fixture that discriminates has to reach the branch while no
// regen is pending, which is the state a virtualized control is in mid-scroll
// and which nothing in the headless harness reproduces yet. Filed as #783;
// until then this comment, not a passing test, is the claim.

namespace
{

// The drain specimen: a zero-HEIGHT, overflow:visible box inside scroll
// content, with a child that has its own size and background. `sizedBox` is
// the control — same shape, real height — so a stale child position below is
// attributable to the zero height and not to a drain that did nothing at all.
struct ZeroSizeScrollFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    ScrollView* Sv = nullptr;
    UIElement* ZeroChild = nullptr;
    UIElement* SizedChild = nullptr;

    bool Init()
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto sv = std::make_unique<ScrollView>();
        Sv = sv.get();
        sv->SetId("sv");
        root->AddChild(std::move(sv));

        // A spacer keeps both specimens off the viewport top, so a modest
        // scroll never clamps them against the ScrollView's own clip.
        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("spacer");
        Sv->GetViewport()->AddChild(std::move(spacer));

        auto zeroBox = std::make_unique<UIElement>();
        zeroBox->SetId("zeroBox");
        zeroBox->AddClass("zerobox");
        auto zeroChild = std::make_unique<UIElement>();
        ZeroChild = zeroChild.get();
        zeroChild->SetId("zeroChild");
        zeroChild->AddClass("inner");
        zeroBox->AddChild(std::move(zeroChild));
        Sv->GetViewport()->AddChild(std::move(zeroBox));

        auto sizedBox = std::make_unique<UIElement>();
        sizedBox->SetId("sizedBox");
        sizedBox->AddClass("sizedbox");
        auto sizedChild = std::make_unique<UIElement>();
        SizedChild = sizedChild.get();
        sizedChild->SetId("sizedChild");
        sizedChild->AddClass("inner");
        sizedBox->AddChild(std::move(sizedChild));
        Sv->GetViewport()->AddChild(std::move(sizedBox));

        auto filler = std::make_unique<UIElement>();
        filler->AddClass("filler");
        Sv->GetViewport()->AddChild(std::move(filler));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));
        // The scroll frame must be attributable to the drain, so the test
        // reads the per-frame regen cause back out of the profile ring.
        Ui->SetUpdateProfilingEnabled(true);

        const auto css = std::filesystem::temp_directory_path() / "ui_zero_size_scroll.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 220px; height: 240px; }\n"
                 "#sv { width: 220px; height: 160px; }\n"
                 ".spacer { width: 100px; height: 40px; flex-shrink: 0; }\n"
                 // The specimen: zero height, children NOT clipped away.
                 ".zerobox { display: flex; flex-direction: column; width: 100px; height: 0px; "
                 "overflow: visible; flex-shrink: 0; }\n"
                 // The control: identical but with a real height.
                 ".sizedbox { display: flex; flex-direction: column; width: 100px; height: 24px; "
                 "overflow: visible; flex-shrink: 0; }\n"
                 ".inner { width: 100px; height: 24px; background-color: #808080; flex-shrink: 0; }\n"
                 ".filler { width: 100px; height: 400px; flex-shrink: 0; }\n";
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

    ~ZeroSizeScrollFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

constexpr float kScrollPx = 30.0f;

} // namespace

TEST(ZeroSizeSubtree, ZeroSizeParentsChildFollowsAScrollDrain)
{
    ZeroSizeScrollFixture fx;
    if (!fx.Init())
        GTEST_SKIP() << "no headless GPU device";

    // Instrument check: the premise is that the child of the zero-height box
    // emits at all. That is what the DFS fix bought; without it the assertions
    // below would be vacuous rather than failing.
    const UI::UIPrimitive* zeroPrimBefore = fx.Ui->PeekPrimitiveForTesting(*fx.ZeroChild, 0);
    ASSERT_NE(zeroPrimBefore, nullptr)
        << "a zero-height parent's child must emit before its drain behaviour can be tested";
    const UI::UIPrimitive* sizedPrimBefore = fx.Ui->PeekPrimitiveForTesting(*fx.SizedChild, 0);
    ASSERT_NE(sizedPrimBefore, nullptr);

    const float zeroBaseY = zeroPrimBefore->Y;
    const float sizedBaseY = sizedPrimBefore->Y;
    const float zeroBaseLayoutY = fx.ZeroChild->GetLayoutY();
    ASSERT_NEAR(zeroBaseY, zeroBaseLayoutY, 0.5f) << "resting primitive sits at the child's rect";

    // Scroll: the content subtree translates up by kScrollPx and drains. No
    // Yoga solve, no full regen.
    fx.Sv->SetScrollY(kScrollPx);
    fx.Pump();
    ASSERT_NEAR(fx.Sv->GetScrollY(), kScrollPx, 0.5f) << "content must have scrolled";

    // Instrument check: a full regen would re-emit every element from the DFS
    // and make both assertions below pass whatever the mark walks did. Pin the
    // frame as a genuine drain frame before reading anything off it.
    UIManager::UpdateProfileFrame prof{};
    ASSERT_TRUE(fx.Ui->GetLastUpdateProfileFrame(prof));
    ASSERT_EQ(prof.GenAllPrimitivesRegenCause, 0u)
        << "scroll translation must stay drain-only; a full regen invalidates this test";
    ASSERT_GT(prof.DrainItems, 0u) << "the scroll frame must actually have drained something";

    // The translation moved both children's committed rects...
    EXPECT_NEAR(fx.ZeroChild->GetLayoutY(), zeroBaseLayoutY - kScrollPx, 0.5f)
        << "scroll translation must move the committed rect regardless of the parent's size";

    // ...and the control proves the drain re-emits translated children at all.
    const UI::UIPrimitive* sizedPrimAfter = fx.Ui->PeekPrimitiveForTesting(*fx.SizedChild, 0);
    ASSERT_NE(sizedPrimAfter, nullptr);
    ASSERT_NEAR(sizedPrimAfter->Y, sizedBaseY - kScrollPx, 0.5f)
        << "control: a sized parent's child re-emits at the scrolled rect";

    // The assertion. A mark walk that stops at the zero-height parent leaves
    // this primitive baked at the pre-scroll Y while its rect has moved — the
    // child renders kScrollPx below where it belongs until something unrelated
    // forces a full regen.
    const UI::UIPrimitive* zeroPrimAfter = fx.Ui->PeekPrimitiveForTesting(*fx.ZeroChild, 0);
    ASSERT_NE(zeroPrimAfter, nullptr);
    EXPECT_NEAR(zeroPrimAfter->Y, zeroBaseY - kScrollPx, 0.5f)
        << "a zero-size parent must not freeze its child's primitives on drain frames";
}

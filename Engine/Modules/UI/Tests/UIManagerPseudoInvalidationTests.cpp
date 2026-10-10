#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#include "Rendering/Core/Device.h"
#include "UI/Controls/Toggle.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

static std::filesystem::path MakeTempCssPath(const char* prefix)
{
    const auto tmpDir = std::filesystem::temp_directory_path();
    const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const uint64_t tid = (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
    return tmpDir / (std::string(prefix) + "_" + std::to_string(t) + "_" + std::to_string(tid) + ".css");
}

TEST(UIManagerPseudoInvalidationTests, HoverPaintOnlyDoesNotTriggerYogaSolve)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // Ensure controls are registered so <uielement> is recognized
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Paint-only hover: background-color changes, no layout-affecting properties.
    const auto cssPath = MakeTempCssPath("ui_hover_paint");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a:hover { background-color: rgb(255, 0, 0); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);

    // Frame 0: baseline layout, no hover.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 1: move mouse over #a (hover changes, dirty is marked post-hit-test).
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 2: hover is now considered during style computation; should NOT require Yoga solve.
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* b = r->FindById("b");
    ASSERT_NE(b, nullptr);

    // Layout should remain unchanged.
    EXPECT_NEAR(b->GetLayoutX(), 100.0f, 0.5f);
    // And Yoga solve should have been skipped for this frame.
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0);
}

TEST(UIManagerPseudoInvalidationTests, HoverLayoutTriggersYogaSolveNextFrame)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Layout-affecting hover: #a grows, pushing #b.
    const auto cssPath = MakeTempCssPath("ui_hover_layout");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
#a:hover { width: 200px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);

    // Frame 0: baseline layout, no hover.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 1: move mouse over #a (hover changes, dirty is marked post-hit-test).
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    const double yogaAfterHoverMove = ui.GetLastUpdateYogaMs();

    // Frame 2: hover is now considered during style computation; should require Yoga solve.
    ui.Update(0.0f, /*interactive=*/true);
    const double yogaAfterHoverSteady = ui.GetLastUpdateYogaMs();

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* b = r->FindById("b");
    ASSERT_NE(b, nullptr);

    EXPECT_NEAR(b->GetLayoutX(), 200.0f, 0.5f);
    // A layout-affecting hover must trigger Yoga at least once. Depending on the chosen
    // input fast-paths, the solve may occur on the same frame as the hover transition
    // (preferred, avoids 1-frame lag) or on the subsequent frame.
    EXPECT_TRUE(yogaAfterHoverMove > 0.0 || yogaAfterHoverSteady > 0.0);
}

TEST(UIManagerPseudoInvalidationTests, HoverChangesResolvedBackgroundColor)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='btn' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_hover_bgcolor");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#btn { width: 100px; height: 40px; background-color: #3A8FFF; }
#btn:hover { background-color: #5AA0FF; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    // Frame 0: baseline, pointer outside.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* btn = r->FindById("btn");
    ASSERT_NE(btn, nullptr);

    const uint32_t baseColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(baseColor, 0xFF3A8FFFu) << "Base color should be #3A8FFF (ARGB)";

    // Frame 1: move pointer over btn (hover detected; dirty marked post-hit-test).
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 2: CSS re-resolves with hover state; background should change.
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t hoverColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(hoverColor, 0xFF5AA0FFu) << "Hover color should be #5AA0FF (ARGB)";

    // Frame 3: move pointer away; hover deactivates.
    ui.OnMouseMove(1000.0f, 1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    // Frame 4: CSS re-resolves without hover.
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t afterLeaveColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(afterLeaveColor, 0xFF3A8FFFu) << "Should revert to base color after hover leaves";
}

TEST(UIManagerPseudoInvalidationTests, HoverAppliesBorderImageSlice)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='btn' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Paint-only :hover border-image. If border-image is missing from the paint
    // classifier, the hover transition marks nothing dirty, the resolved style never
    // re-resolves, and the override never appears -- this guards that classification.
    const auto cssPath = MakeTempCssPath("ui_hover_borderimage");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#btn { width: 100px; height: 40px; }
#btn:hover { border-image-slice: 12 fill; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));
    ui.SetUpdateProfilingEnabled(true);

    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* btn = r->FindById("btn");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->GetResolvedStyle().Visual.BackgroundImage.HasBorderImage);

    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    const auto& bi = btn->GetResolvedStyle().Visual.BackgroundImage;
    EXPECT_TRUE(bi.HasBorderImage) << "hover border-image-slice must re-resolve (paint classification)";
    EXPECT_NEAR(bi.BiSlice[0], 12.0f, 0.01f);
    // border-image is paint-only -- it must not trigger a Yoga layout solve.
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0);
}

TEST(UIManagerPseudoInvalidationTests, HoverWithOverridesDoesNotStompBackground)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='btn' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_hover_overrides");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
.btn { width: 100px; height: 40px; background-color: #3A8FFF; }
.btn:hover { background-color: #5AA0FF; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* btn = r->FindById("btn");
    ASSERT_NE(btn, nullptr);

    btn->AddClass("btn");
    btn->Overrides()
        .Set(Style::PaddingTop, StyleLength::Px(8.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::Cursor, CursorStyle::Pointer)
        .Set(Style::FontSize, StyleLength::Px(16.0f));

    // Frame 0: baseline.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t baseColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(baseColor, 0xFF3A8FFFu) << "Base color from CSS class";

    // Frame 1: hover
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    // Frame 2: CSS re-resolves with hover
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t hoverColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(hoverColor, 0xFF5AA0FFu) << "Hover color should apply even with unrelated overrides";
}

TEST(UIManagerPseudoInvalidationTests, HoverWithCssVariablesAndMultipleClasses)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='btn' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_hover_vars");
    {
        std::ofstream f(cssPath);
        f << R"(
:root {
    --accent: #3A8FFF;
    --accent-hover: #5AA0FF;
}
#root { display: flex; width: 300px; height: 40px; }
.button.primary { width: 100px; height: 40px; background-color: var(--accent); }
.button.primary:hover { background-color: var(--accent-hover); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* btn = r->FindById("btn");
    ASSERT_NE(btn, nullptr);

    btn->AddClass("button");
    btn->AddClass("primary");

    // Frame 0: baseline.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t baseColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(baseColor, 0xFF3A8FFFu) << "Base color from CSS variable";

    // Frame 1: hover
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    // Frame 2: CSS re-resolves with hover
    ui.Update(0.0f, /*interactive=*/true);

    const uint32_t hoverColor = btn->GetResolvedStyle().Visual.BackgroundColor;
    EXPECT_EQ(hoverColor, 0xFF5AA0FFu) << "Hover color from CSS variable";
}

TEST(UIElementDirtyFlagsTests, ClassChangeMarksSubtreeDirty)
{
    auto root = std::make_unique<UIElement>();
    auto p1 = std::make_unique<UIElement>();
    auto p2 = std::make_unique<UIElement>();
    auto c1 = std::make_unique<UIElement>();

    UIElement* p1Ptr = p1.get();
    UIElement* p2Ptr = p2.get();
    UIElement* c1Ptr = c1.get();

    p1->AddChild(std::move(c1));
    root->AddChild(std::move(p1));
    root->AddChild(std::move(p2));

    // Reset any dirtiness introduced by AddChild so we test only AddClass().
    root->ClearDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
    p1Ptr->ClearDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
    p2Ptr->ClearDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
    c1Ptr->ClearDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);

    p1Ptr->AddClass("hot");

    const unsigned expected = UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty;
    EXPECT_TRUE(p1Ptr->IsDirty(expected));
    EXPECT_TRUE(c1Ptr->IsDirty(expected));

    // Sibling subtree should not be affected.
    EXPECT_FALSE(p2Ptr->IsDirty(expected));
}

// ---------------------------------------------------------------------------
// Focus-family marker generalization (RebuildFocusWithinChain).
//
// RebuildFocusWithinChain used to raise StyleDirty only on the chain elements
// and never consult the style analysis; every other focus/active marker already
// consulted it and raised LayoutDirty when the pseudo affects layout. The
// generalization routes all of them through one analysis-driven decision
// (MarkPseudoStateScope). These guard that a programmatic focus change still
// re-cascades AND relayouts the focused element through the generalized markers.
//
// Scope note (see the branch report): only `:focus` is exercised here because
// (1) the vendored lexbor build drops `:focus-within`/`:focus-visible` selectors
// at parse time, and (2) the cascade matcher only evaluates :focus/:focus-within
// on the selector's target element (isTarget) — an ancestor `:focus`/`:focus-
// within` in a descendant selector never matches (only :hover has ancestor-chain
// semantics). Both are separate, out-of-scope engine gaps; the generalization is
// pseudo-agnostic and covers :focus-within identically once they are fixed.
// ---------------------------------------------------------------------------

TEST(UIManagerPseudoInvalidationTests, ProgrammaticFocusChainElementLayoutRelaysOut)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='container'>
            <uielement id='child' />
        </uielement>
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    // Layout-affecting rule on the focused element itself. Padding shifts the
    // child's origin — proves the chain element relayouts (not just repaints).
    const auto cssPath = MakeTempCssPath("ui_focus_chain");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 80px; }
#container { display: flex; flex-direction: column; width: 300px; height: 80px; }
#child { width: 100px; height: 40px; }
#container:focus { padding-left: 30px; }
)";
    }

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* child = r->FindById("child");
    ASSERT_NE(child, nullptr);
    r->FindById("container")->SetFocusable(true);

    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NEAR(child->GetLayoutX(), 0.0f, 0.5f);

    ui.SetFocusById("container");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(child->GetLayoutX(), 30.0f, 0.5f)
        << "Layout-affecting :focus on the focused element must relayout its subtree";
}

TEST(UIManagerPseudoInvalidationTests, ProgrammaticFocusChainElementLayoutRevertsOnBlur)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='container'>
            <uielement id='child' />
        </uielement>
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    const auto cssPath = MakeTempCssPath("ui_focus_chain_blur");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 80px; }
#container { display: flex; flex-direction: column; width: 300px; height: 80px; }
#child { width: 100px; height: 40px; }
#container:focus { padding-left: 30px; }
)";
    }

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* child = r->FindById("child");
    ASSERT_NE(child, nullptr);
    r->FindById("container")->SetFocusable(true);

    ui.Update(0.0f, /*interactive=*/true);
    ui.SetFocusById("container");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_NEAR(child->GetLayoutX(), 30.0f, 0.5f) << "precondition: focus pads the container";

    // Blur: container leaves the chain. The symmetric-difference "left the chain"
    // mark is the only path that strips the styling on a programmatic blur
    // (SetFocusById("") marks nothing itself).
    ui.SetFocusById("");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(child->GetLayoutX(), 0.0f, 0.5f)
        << "Layout-affecting :focus must revert on programmatic blur";
}

// ---------------------------------------------------------------------------
// :checked and custom-state marker generalization (issue #216).
//
// A Toggle/Checkbox value flip used to mark self LayoutDirty unconditionally
// (Field<bool>::SetValue), and AddCustomState/RemoveCustomState marked a blind
// self StyleDirty|VisualDirty. Neither consulted the style analysis, so a
// paint-only rule still forced a Yoga solve while a layout- or descendant-
// affecting rule was reached only via the signature fallback. Both now route
// through MarkPseudoStateScope (the #211 focus-family path), keyed on new
// Checked / CustomState PseudoInfo.
//
// Both pseudo families are fully reachable. lexbor recognizes :checked and the
// matcher evaluates it on ancestors too (via IsPseudoChecked), so
// `toggle:checked .toggle-knob` matches and its descendant fan-out is exercised
// directly. Custom `:name` states also reach the runtime now that the lexbor
// overlay preserves unknown pseudo-classes (patch 0002): a rule like
// `#el:mystate` parses into a Kind::Custom pseudo, populates the aggregate
// CustomState PseudoInfo, and AddCustomState/RemoveCustomState fan the mark out
// per its analysis-derived bits — exercised end-to-end below.
// ---------------------------------------------------------------------------

TEST(UIManagerPseudoInvalidationTests, CheckedPaintOnlyDoesNotTriggerYogaSolve)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto toggle = std::make_unique<Toggle>();
    Toggle* tg = toggle.get();
    root->AddChild(std::move(toggle));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Paint-only :checked: background changes, nothing layout-affecting.
    const auto cssPath = MakeTempCssPath("ui_checked_paint");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
.toggle { width: 36px; height: 20px; background-color: rgb(80, 80, 80); }
.toggle:checked { background-color: rgb(255, 0, 0); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ui.Update(0.0f, /*interactive=*/true); // baseline layout

    tg->SetChecked(true);
    ui.Update(0.0f, /*interactive=*/true); // :checked applies
    ui.Update(0.0f, /*interactive=*/true); // settle

    EXPECT_EQ(tg->GetResolvedStyle().Visual.BackgroundColor, 0xFFFF0000u)
        << ":checked background must re-resolve";
    EXPECT_NEAR(tg->GetLayoutWidth(), 36.0f, 0.5f);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0)
        << "paint-only :checked must not trigger a Yoga solve";
}

TEST(UIManagerPseudoInvalidationTests, CheckedLayoutTriggersYogaSolve)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto toggle = std::make_unique<Toggle>();
    Toggle* tg = toggle.get();
    root->AddChild(std::move(toggle));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Layout-affecting :checked: the toggle widens on check.
    const auto cssPath = MakeTempCssPath("ui_checked_layout");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
.toggle { width: 36px; height: 20px; }
.toggle:checked { width: 120px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_NEAR(tg->GetLayoutWidth(), 36.0f, 0.5f) << "precondition: unchecked width";

    tg->SetChecked(true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(tg->GetLayoutWidth(), 120.0f, 0.5f)
        << "layout-affecting :checked must relayout the toggle";
}

TEST(UIManagerPseudoInvalidationTests, CheckedDescendantLayoutRelaysOut)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto toggle = std::make_unique<Toggle>();
    Toggle* tg = toggle.get();
    root->AddChild(std::move(toggle));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Descendant-combinator :checked rule targeting the toggle knob. The
    // matcher evaluates :checked on the ancestor toggle, so this matches; the
    // MayAffectDescendants fan-out must reach the knob and relayout it.
    const auto cssPath = MakeTempCssPath("ui_checked_desc");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
.toggle { display: flex; width: 60px; height: 20px; }
.toggle-knob { width: 10px; height: 10px; }
.toggle:checked .toggle-knob { width: 30px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    UIElement* knob = tg->GetChildren().empty() ? nullptr : tg->GetChildren().front().get();
    ASSERT_NE(knob, nullptr);
    ASSERT_NEAR(knob->GetLayoutWidth(), 10.0f, 0.5f) << "precondition: unchecked knob width";

    tg->SetChecked(true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(knob->GetLayoutWidth(), 30.0f, 0.5f)
        << "descendant :checked rule must fan out and relayout the knob";
}

TEST(UIManagerPseudoInvalidationTests, CustomStatePaintOnlyRestylesWithoutYogaSolve)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto el = std::make_unique<UIElement>("el");
    UIElement* elPtr = el.get();
    root->AddChild(std::move(el));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Paint-only custom state: background changes, nothing layout-affecting.
    // Now that `#el:mystate` reaches the runtime, AddCustomState must re-resolve
    // style but skip the Yoga solve (the CustomState PseudoInfo is paint-only).
    const auto cssPath = MakeTempCssPath("ui_customstate_paint");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#el { width: 40px; height: 20px; background-color: rgb(80, 80, 80); }
#el:mystate { background-color: rgb(255, 0, 0); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ui.Update(0.0f, /*interactive=*/true); // baseline
    ASSERT_EQ(elPtr->GetResolvedStyle().Visual.BackgroundColor, 0xFF505050u);

    elPtr->AddCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true); // :mystate applies
    ui.Update(0.0f, /*interactive=*/true); // settle

    EXPECT_EQ(elPtr->GetResolvedStyle().Visual.BackgroundColor, 0xFFFF0000u)
        << ":mystate background must re-resolve — custom-state rules now reach the runtime";
    EXPECT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0)
        << "paint-only :mystate must not trigger a Yoga solve";

    elPtr->RemoveCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(elPtr->GetResolvedStyle().Visual.BackgroundColor, 0xFF505050u)
        << "removing :mystate must revert the background";
}

TEST(UIManagerPseudoInvalidationTests, CustomStateLayoutTriggersYogaSolve)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto el = std::make_unique<UIElement>("el");
    UIElement* elPtr = el.get();
    root->AddChild(std::move(el));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Layout-affecting custom state: the element widens when the state is set.
    const auto cssPath = MakeTempCssPath("ui_customstate_layout");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#el { width: 40px; height: 20px; }
#el:mystate { width: 120px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f) << "precondition: idle width";

    elPtr->AddCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(elPtr->GetLayoutWidth(), 120.0f, 0.5f)
        << "layout-affecting :mystate must relayout the element";

    elPtr->RemoveCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f)
        << "removing :mystate must revert the layout";
}

TEST(UIManagerPseudoInvalidationTests, CustomStateDescendantLayoutRelaysOut)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='panel'>
        <uielement id='spinner' />
    </uielement>)";
    std::unique_ptr<UIElement> panel;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, panel));
    UIElement* panelPtr = panel.get();

    UIManager ui(dev);
    ui.SetRoot(std::move(panel));

    // Descendant-combinator custom-state rule targeting the spinner. The
    // matcher evaluates :loading on the ancestor panel (via HasCustomState), so
    // the MayAffectDescendants fan-out must reach the spinner and relayout it.
    const auto cssPath = MakeTempCssPath("ui_customstate_desc");
    {
        std::ofstream f(cssPath);
        f << R"(
#panel { display: flex; width: 300px; height: 40px; }
#spinner { width: 10px; height: 10px; }
#panel:loading #spinner { width: 30px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    UIElement* spinner = panelPtr->GetChildren().empty() ? nullptr : panelPtr->GetChildren().front().get();
    ASSERT_NE(spinner, nullptr);
    ASSERT_NEAR(spinner->GetLayoutWidth(), 10.0f, 0.5f) << "precondition: idle spinner width";

    panelPtr->AddCustomState("loading");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_NEAR(spinner->GetLayoutWidth(), 30.0f, 0.5f)
        << "descendant :loading rule must fan out and relayout the spinner";

    panelPtr->RemoveCustomState("loading");
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NEAR(spinner->GetLayoutWidth(), 10.0f, 0.5f)
        << "removing :loading must revert the spinner";
}

TEST(UIManagerPseudoInvalidationTests, CustomStateAddRemoveIsSafeNoOpWithoutMatchingRules)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto el = std::make_unique<UIElement>("el");
    UIElement* elPtr = el.get();
    root->AddChild(std::move(el));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // The stylesheet has no custom-state rule, so the aggregate CustomState
    // PseudoInfo affects neither layout nor paint. MarkCustomStateScope must
    // then be a safe no-op — no crash, no spurious relayout, no Yoga solve.
    const auto cssPath = MakeTempCssPath("ui_customstate_noop");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#el { width: 40px; height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.SetUpdateProfilingEnabled(true);
    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f);

    elPtr->AddCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0)
        << "custom-state add with no matching rule must not solve Yoga";

    elPtr->RemoveCustomState("mystate");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NEAR(elPtr->GetLayoutWidth(), 40.0f, 0.5f);
    EXPECT_DOUBLE_EQ(ui.GetLastUpdateYogaMs(), 0.0)
        << "custom-state remove with no matching rule must not solve Yoga";
}

// Regression: hover rules (including descendant rules like
// `.card:hover .overlay`) must restyle subtrees that were added at RUNTIME —
// after the initial build — exactly like constructor-built ones. Mirrors the
// project picker's community grid, whose cards are created when an async
// catalog fetch lands (and whose grid is display:none until that same frame).
namespace
{

std::unique_ptr<UIElement> MakePseudoCard(const char* id)
{
    auto card = std::make_unique<UIElement>(id);
    card->AddClass("card");
    auto overlay = std::make_unique<UIElement>();
    overlay->AddClass("overlay");
    overlay->Overrides().Set(Style::PointerEvents, false);
    card->AddChild(std::move(overlay));
    return card;
}

constexpr uint32_t kOverlayHoverColor = 0xFFFF0000u;

void ExpectCardHoverWorks(UIManager& ui, const char* cardId, float cardCenterX,
                          const char* what)
{
    UIElement* card = ui.GetRootElement()->FindById(cardId);
    ASSERT_NE(card, nullptr) << what;
    ASSERT_FALSE(card->GetChildren().empty()) << what;
    UIElement* overlay = card->GetChildren().front().get();

    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_NE(overlay->GetResolvedStyle().Visual.BackgroundColor, kOverlayHoverColor)
        << what << ": overlay already hover-colored before hover";

    ui.OnMouseMove(cardCenterX, 20.0f);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(overlay->GetResolvedStyle().Visual.BackgroundColor, kOverlayHoverColor)
        << what << ": .card:hover .overlay did not apply";

    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_NE(overlay->GetResolvedStyle().Visual.BackgroundColor, kOverlayHoverColor)
        << what << ": hover style did not clear on leave";
}

} // namespace

TEST(UIManagerPseudoInvalidationTests, HoverDescendantRuleAppliesToRuntimeAddedSubtree)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto grid = std::make_unique<UIElement>("grid");
    UIElement* gridPtr = grid.get();
    grid->AddChild(MakePseudoCard("card-static"));
    root->AddChild(std::move(grid));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_hover_runtime_subtree");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 600px; height: 60px; }
#grid { display: flex; flex-direction: row; width: 600px; height: 60px; }
.card { width: 100px; height: 40px; background-color: #202020; }
.card .overlay { width: 100%; height: 100%; }
.card:hover .overlay { background-color: #FF0000; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    // Settle a few frames so the runtime additions below happen against a
    // fully built, clean tree (mirrors the editor's steady state).
    for (int i = 0; i < 3; ++i)
        ui.Update(0.0f, /*interactive=*/true);

    // Control: constructor-built card hover-restyles.
    ExpectCardHoverWorks(ui, "card-static", 50.0f, "ctor-built card");

    // Runtime add into the live grid (recents-rebuild shape).
    gridPtr->AddChild(MakePseudoCard("card-runtime"));
    ui.Update(0.0f, /*interactive=*/true);
    ExpectCardHoverWorks(ui, "card-runtime", 150.0f, "runtime-added card");

    // Runtime add while the grid is display:none, flipped visible the same
    // frame (community-grid shape: cards rebuilt when the fetch lands, the
    // status view swap happens right after, both before the next Update).
    gridPtr->Overrides().Set(Style::Display, DisplayMode::None);
    ui.Update(0.0f, /*interactive=*/true);
    gridPtr->AddChild(MakePseudoCard("card-hidden-add"));
    gridPtr->Overrides().Set(Style::Display, DisplayMode::Flex);
    ui.Update(0.0f, /*interactive=*/true);
    ExpectCardHoverWorks(ui, "card-hidden-add", 250.0f, "hidden-then-shown card");
}

// Same scenario as above, plus the two ingredients the project picker adds:
// the hover-dependent property declares a CSS TRANSITION, and an unrelated
// element churns an override every frame (the picker's logo hue animation),
// so every frame takes the heavy cascade path.
TEST(UIManagerPseudoInvalidationTests, HoverDescendantTransitionAppliesToRuntimeAddedSubtreeUnderChurn)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>("root");
    auto churn = std::make_unique<UIElement>("churn");
    UIElement* churnPtr = churn.get();
    root->AddChild(std::move(churn));
    auto grid = std::make_unique<UIElement>("grid");
    UIElement* gridPtr = grid.get();
    grid->AddChild(MakePseudoCard("card-static"));
    root->AddChild(std::move(grid));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_hover_runtime_transition");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; flex-direction: column; width: 600px; height: 120px; }
#churn { width: 10px; height: 10px; }
#grid { display: flex; flex-direction: row; width: 600px; height: 60px; }
.card { width: 100px; height: 40px; background-color: #202020; }
.card .overlay { width: 100%; height: 100%; transition: background-color 0.16s ease-out; }
.card:hover .overlay { background-color: #FF0000; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    // Every Update tick below also churns an override, like the picker's
    // per-frame logo hue rotation.
    uint32_t hue = 0;
    auto tick = [&]() {
        churnPtr->Overrides().Set(Style::BackgroundColor, 0xFF000000u | hue++);
        churnPtr->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        ui.Update(0.016f, /*interactive=*/true);
    };

    for (int i = 0; i < 3; ++i)
        tick();

    auto expectHoverTintWorks = [&](const char* cardId, float centerX, const char* what) {
        UIElement* card = ui.GetRootElement()->FindById(cardId);
        ASSERT_NE(card, nullptr) << what;
        UIElement* overlay = card->GetChildren().front().get();

        ui.OnMouseMove(-1000.0f, -1000.0f);
        for (int i = 0; i < 20; ++i)
            tick();
        const uint32_t before = overlay->GetResolvedStyle().Visual.BackgroundColor;
        ASSERT_NE(before, kOverlayHoverColor) << what;

        // Grid row starts at y=10 (below #churn); card centers at y=30.
        ui.OnMouseMove(centerX, 30.0f);
        for (int i = 0; i < 30; ++i) // > 0.16s of 16ms frames: transition finishes
            tick();
        EXPECT_EQ(overlay->GetResolvedStyle().Visual.BackgroundColor, kOverlayHoverColor)
            << what << ": .card:hover .overlay transition target not reached";
    };

    expectHoverTintWorks("card-static", 50.0f, "ctor-built card under churn");

    gridPtr->Overrides().Set(Style::Display, DisplayMode::None);
    tick();
    gridPtr->AddChild(MakePseudoCard("card-hidden-add"));
    gridPtr->Overrides().Set(Style::Display, DisplayMode::Flex);
    tick();
    expectHoverTintWorks("card-hidden-add", 150.0f, "hidden-then-shown card under churn");
}

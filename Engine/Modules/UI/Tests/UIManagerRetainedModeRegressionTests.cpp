#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "Core/Application.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"
#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct SimpleGridProvider final : GridChangeTrackingProvider
{
    explicit SimpleGridProvider(int count)
    {
        m_Ids.reserve(std::max(0, count));
        for (int i = 0; i < count; ++i)
            m_Ids.push_back((GridId)(i + 1));
    }

    int GetItemCount() const override { return (int)m_Ids.size(); }
    GridId GetItemId(int index) const override
    {
        if (index < 0 || index >= (int)m_Ids.size())
            return 0;
        return m_Ids[(size_t)index];
    }
    const char* GetLabel(GridId) const override { return "Item"; }
    uint64_t GetIcon(GridId) const override { return 0; }
    const char* GetTypeKey(GridId) const override { return ""; }
    void ApplySort(const SortDescriptor&) override {}
    void ApplyGrouping(const GroupDescriptor&) override {}

    std::vector<GridId> m_Ids;
};

class ToggleMeasuredElement final : public UIElement, public ITextMeasurable
{
  public:
    ToggleMeasuredElement() = default;

    void SetMeasureEnabled(bool enabled) { m_MeasureEnabled = enabled; }

    void GetTextMeasureInfo(TextMeasureInfo& info, const ResolvedStyle&) const override
    {
        info.HasText = m_MeasureEnabled;
        if (m_MeasureEnabled)
            info.Text = "Hello";
        else
            info.Text.clear();
        info.ExplicitFontSize = 0.0f;
    }

  private:
    bool m_MeasureEnabled = false;
};

} // namespace

TEST(UIRegressions, TooltipWrapsAtMaximumWidthAndShortTextReturnsToNaturalWidth)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto source = std::make_unique<UIElement>();
    UIElement* sourcePtr = source.get();
    source->SetId("source");
    source->SetTooltip(
        "Anything below the near clipping distance is not rendered. This deliberately long "
        "tooltip must wrap across multiple rows while preserving enough height for every line.");
    root->AddChild(std::move(source));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Mirrors the editor's widgets.css tooltip rules. Deliberately no width
    // on .tooltip-text and no max-width anywhere: the wrap cap is the
    // engine-side Style::MaxWidth override TooltipOverlay installs on
    // #ui-tooltip, and a percent width on the label would let long text
    // escape the bubble (measure at pre-clamp width) instead of wrapping.
    const auto css = std::filesystem::temp_directory_path() / "ui_regression_tooltip_wrap.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 1000px; height: 600px; }
#source { width: 100px; height: 30px; }
.tooltip-wrapper { display: none; position: absolute; flex-direction: column; align-items: flex-start; }
.tooltip { display: flex; flex-direction: column; align-items: center; padding: 6px 10px; min-width: 60px; overflow: hidden; }
.tooltip-text { display: flex; font-size: 14px; line-height: 20px; padding: 1px 0 0 0; overflow: hidden; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootPtr = ui.GetRootElement();
    ASSERT_NE(rootPtr, nullptr);

    UI::Interaction::TooltipOverlay overlay;
    overlay.SetHoverDelay(0.0f);

    // Frame 1: new text parks the bubble; the next layout pass measures the
    // label against the max-width cap (wrapped size lands in one pass).
    overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);
    ui.Update(0.0f, /*interactive=*/false);
    // Frame 2: dimensions are final; the tooltip becomes visible.
    overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);

    UIElement* wrapper = rootPtr->FindById("ui-tooltip-wrapper");
    UIElement* tooltip = rootPtr->FindById("ui-tooltip");
    UIElement* label = rootPtr->FindById("ui-tooltip-text");
    ASSERT_NE(wrapper, nullptr);
    ASSERT_NE(tooltip, nullptr);
    ASSERT_NE(label, nullptr);
    EXPECT_GE(wrapper->GetLayoutX(), 0.0f) << "tooltip did not un-park after one measure frame";
    EXPECT_LE(tooltip->GetLayoutWidth(), 512.5f);
    EXPECT_GT(tooltip->GetLayoutWidth(), 350.0f) << "long tooltip did not fill toward the cap";
    EXPECT_GT(label->GetLayoutHeight(), 20.0f) << "long tooltip remained a single line";
    EXPECT_GT(tooltip->GetLayoutHeight(), 32.0f) << "tooltip body did not grow for wrapped text";
    EXPECT_GE(tooltip->GetLayoutHeight(), label->GetLayoutHeight() + 11.0f)
        << "tooltip body is shorter than its wrapped text plus vertical padding";
    EXPECT_LE(label->GetLayoutWidth(), tooltip->GetLayoutWidth() - 19.0f)
        << "wrapped label escaped the tooltip content box";

    // The max width is a cap, not sticky: inspector-sized help remains compact.
    sourcePtr->SetTooltip("Near clipping plane distance in world units.");
    overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);
    ui.Update(0.0f, /*interactive=*/false);
    overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);

    EXPECT_LT(tooltip->GetLayoutWidth(), 512.0f);
    EXPECT_LE(label->GetLayoutHeight(), 21.2f); // one "normal" line box (21.1016)
}

// The hover tooltip lives on OverlayLayer::HoverTooltip, the final overlay
// layer, so its bubble draws above dropdown/modal/drag-preview overlays even
// though those escape their panels' stacking contexts too.
TEST(UIRegressions, HoverTooltipDrawsAboveDropdownOverlay)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UIRegistration::RegisterBuiltInControls();
        UiRgHarness rg(dev);

        auto root = std::make_unique<UIElement>();
        root->SetId("root");

        auto source = std::make_unique<UIElement>();
        UIElement* sourcePtr = source.get();
        source->SetId("source");
        source->SetTooltip("Hover help that must draw above the dropdown.");
        root->AddChild(std::move(source));

        auto dropdown = std::make_unique<UIElement>();
        UIElement* dropdownPtr = dropdown.get();
        dropdown->SetId("dropdown");
        dropdown->SetOverlayLayer(OverlayLayer::Dropdown);
        root->AddChild(std::move(dropdown));

        UIManager ui(dev);
        ui.SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_regression_tooltip_overlay_order.css";
        {
            std::ofstream f(css);
            f << R"(
#root { display: flex; width: 1000px; height: 600px; }
#source { width: 100px; height: 30px; }
#dropdown { position: absolute; left: 20px; top: 40px; width: 300px; height: 200px; background-color: #223344; }
.tooltip-wrapper { display: none; position: absolute; flex-direction: column; align-items: flex-start; }
.tooltip { display: flex; flex-direction: column; align-items: center; padding: 6px 10px; min-width: 60px; overflow: hidden; background-color: #121212; }
.tooltip-text { display: flex; font-size: 14px; line-height: 20px; padding: 1px 0 0 0; overflow: hidden; }
)";
        }
        ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
        ui.Update(0.0f, /*interactive=*/false);

        UIElement* rootPtr = ui.GetRootElement();
        ASSERT_NE(rootPtr, nullptr);

        UI::Interaction::TooltipOverlay overlay;
        overlay.SetHoverDelay(0.0f);
        overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);
        ui.Update(0.0f, /*interactive=*/false);
        overlay.Update(rootPtr, sourcePtr, 50.0f, 20.0f, 3.0f);
        ui.Update(0.0f, /*interactive=*/false);
        DriveUiRender(ui, rg);

        UIElement* bubble = rootPtr->FindById("ui-tooltip");
        ASSERT_NE(bubble, nullptr);

        const int dropdownPos = ui.FindDrawOrderPosForTesting(*dropdownPtr);
        const int bubblePos = ui.FindDrawOrderPosForTesting(*bubble);
        ASSERT_GE(dropdownPos, 0) << "dropdown overlay emitted no primitives";
        ASSERT_GE(bubblePos, 0) << "tooltip bubble emitted no primitives (not visible?)";
        EXPECT_GT(bubblePos, dropdownPos)
            << "hover tooltip bubble drew beneath the dropdown overlay";
    }
}

TEST(UIRegressions, GridViewInitialColumnsUsesUpdatedViewportOnFirstUpdate)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto gv = std::make_unique<GridView>();
    GridView* gvPtr = gv.get();
    gv->SetId("grid");

    SimpleGridProvider provider(64);
    gv->SetDataProvider(&provider);

    root->AddChild(std::move(gv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_grid_initial_cols.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 1000px; height: 300px; }
#grid { width: 1000px; height: 300px; }
.scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
.scroll-viewport { flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
.scroll-content { min-width: 0; min-height: 0; }
.hidden { display: none; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);

    // Infer column count by checking whether item 9 is still on the first row.
    // With width=1000 and default GridView cell sizing (96px + 8px gap), we
    // expect >=9 columns, so item 1 and item 9 should share the same Y.
    const uint64_t gvAddr = (uint64_t)(std::uintptr_t)gvPtr;
    const std::string cell1Id = std::string("gridcell-") + std::to_string(gvAddr) + "-1";
    const std::string cell9Id = std::string("gridcell-") + std::to_string(gvAddr) + "-9";

    UIElement* cell1 = rootEl->FindById(cell1Id);
    UIElement* cell9 = rootEl->FindById(cell9Id);
    ASSERT_NE(cell1, nullptr);
    ASSERT_NE(cell9, nullptr);

    EXPECT_NEAR(cell1->GetLayoutY(), cell9->GetLayoutY(), 0.5f);
}

TEST(UIRegressions, CaptureSurvivesTreeChangedRebuild)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto cap = std::make_unique<UIElement>(); // intentionally no id
    UIElement* capPtr = cap.get();
    auto other = std::make_unique<UIElement>();
    other->SetId("other");
    UIElement* otherPtr = other.get();

    int moveCount = 0;
    int otherMoveCount = 0;
    int otherEnterCount = 0;
    cap->RegisterEventHandler(kEventMouseMove, [&](UIEvent&) { ++moveCount; });
    other->RegisterEventHandler(kEventMouseMove, [&](UIEvent&) { ++otherMoveCount; });
    other->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++otherEnterCount; });
    cap->RegisterEventHandler(kEventMouseDown, [&](UIEvent& e)
                              {
                                  e.Capture(capPtr);
                                  // Force treeChanged rebuild path by posting a deferred action.
                                  capPtr->PostAction([]() {});
                              });

    root->AddChild(std::move(cap));
    root->AddChild(std::move(other));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_capture_rebuild.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 200px; }
uielement { width: 100px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Layout once.
    ui.Update(0.0f, /*interactive=*/false);

    // Press inside the element so it becomes the click target and requests capture.
    ui.OnMouseMove(10.0f, 10.0f);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);

    // Capture should have forced a stable id for restoration.
    EXPECT_FALSE(capPtr->GetId().empty());

    const int moveCountAfterPress = moveCount;

    // Move over the other element. Capture keeps the captured element as the
    // target and hover boundary until release.
    // This stable captured move should also use the pointer-only update path:
    // capture changes event routing, but does not require layout or cascade.
    ui.SetUpdateProfilingEnabled(true);
    ui.OnMouseMove(150.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_GT(moveCount, moveCountAfterPress);
    EXPECT_EQ(otherMoveCount, 0);
    EXPECT_EQ(otherEnterCount, 0);
    EXPECT_EQ(ui.GetHoveredElement(), capPtr);
    UIManager::UpdateProfileFrame profile{};
    ASSERT_TRUE(ui.GetLastUpdateProfileFrame(profile));
    EXPECT_DOUBLE_EQ(profile.BuildYogaMs, 0.0);
    EXPECT_DOUBLE_EQ(profile.YogaMs, 0.0);

    // Releasing capture restores the physical hover target without requiring
    // another mouse-position callback.
    ui.OnMouseButton(0, false);
    ui.Update(0.0f, /*interactive=*/true);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(ui.GetHoveredElement(), otherPtr);
    EXPECT_GT(otherEnterCount, 0);
}

TEST(UIRegressions, SetValueSkipsLayoutForFixedTextInput)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto input = std::make_unique<TextInput>();
    input->SetId("input");
    TextInput* inputPtr = input.get();
    root->AddChild(std::move(input));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() /
                     "ui_regression_fixed_text_set_value.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 200px; height: 100px; }
#input { width: 100%; height: 26px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));
    ui.Update(0.0f, /*interactive=*/false);

    ui.SetUpdateProfilingEnabled(true);
    inputPtr->SetValue("123.456");
    EXPECT_FALSE(inputPtr->IsDirty(UIElement::LayoutDirty));
    ui.Update(0.0f, /*interactive=*/false);

    UIManager::UpdateProfileFrame profile{};
    ASSERT_TRUE(ui.GetLastUpdateProfileFrame(profile));
    EXPECT_DOUBLE_EQ(profile.BuildYogaMs, 0.0);
    EXPECT_DOUBLE_EQ(profile.YogaMs, 0.0);
}

TEST(UIRegressions, SetValueKeepsLayoutForIntrinsicTextInput)
{
    TextInput input;
    input.SetValue("content-sized");
    EXPECT_TRUE(input.IsDirty(UIElement::LayoutDirty));
}

TEST(UIRegressions, CompositeSetValueKeepsTheBaseDirtyContract)
{
    constexpr unsigned kDirtyFlags =
        UIElement::StyleDirty | UIElement::LayoutDirty |
        UIElement::VisualDirty | UIElement::ChildrenDirty |
        UIElement::SubtreeDirty;

    FloatField scalar;
    scalar.ClearDirty(kDirtyFlags);
    scalar.SetValue(42.0f);
    EXPECT_FLOAT_EQ(scalar.GetValue(), 42.0f);
    EXPECT_TRUE(scalar.IsDirty(UIElement::VisualDirty));
    EXPECT_FALSE(scalar.IsDirty(UIElement::LayoutDirty));

    scalar.ClearDirty(kDirtyFlags);
    scalar.SetValueWithoutNotify(84.0f);
    EXPECT_FLOAT_EQ(scalar.GetValue(), 84.0f);
    EXPECT_TRUE(scalar.IsDirty(UIElement::VisualDirty));
    EXPECT_FALSE(scalar.IsDirty(UIElement::LayoutDirty));

    Vector3Field vector;
    vector.ClearDirty(kDirtyFlags);
    const Rendering::Vector3 expected(1.0f, 2.0f, 3.0f);
    vector.SetValue(expected);
    EXPECT_FLOAT_EQ(vector.GetValue().x, expected.x);
    EXPECT_FLOAT_EQ(vector.GetValue().y, expected.y);
    EXPECT_FLOAT_EQ(vector.GetValue().z, expected.z);
    EXPECT_TRUE(vector.IsDirty(UIElement::VisualDirty));
    EXPECT_FALSE(vector.IsDirty(UIElement::LayoutDirty));
}

// An empty label with no background emits nothing, so it owns no primitive
// slot range. With both axes pinned to px, the first SetText takes the
// content-only pipeline (MarkContentDirty), whose drain can only rewrite
// slots an element already owns — the producer must escalate a rangeless
// element to a full regen (cause 0x800), or the text never paints for as
// long as nothing else dirties the tree.
//
// The root's background-color is load-bearing: it keeps the DrawOrder
// non-empty, which is a precondition of the drain path. In an all-empty tree
// the manager falls back to a full regen and this test cannot distinguish
// the escalation from the fallback.
TEST(UIRegressions, FirstTextOnAnEmptyFixedSizeLabelPaints)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
        R"(<uielement id="root"><label id="status"/></uielement>)",
        R"(
#root { display: flex; width: 400px; height: 100px; background-color: #202020; }
#status { width: 200px; height: 24px; font-family: Roboto; font-size: 16px; color: #ffffff; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    auto* label = dynamic_cast<Label*>(fx.Element("status"));
    ASSERT_NE(label, nullptr);

    // MarkPrimitivesNeedRegen stamps causes into the last PUSHED profile
    // frame, and PublishUpdateProfile only pushes at the end of an Update
    // that ran with profiling enabled — so profiling must be on AND one
    // profiled frame must have completed before SetText, or the
    // escalation's stamp lands nowhere and this pin reads zero.
    fx.Manager().SetUpdateProfilingEnabled(true);
    fx.StepFrame();

    label->SetText("now visible");
    // The escalation stamps its cause into the profile frame current at the
    // SetText call, so that frame must be sampled before stepping pushes it
    // out of GetLastUpdateProfileFrame's reach.
    uint32_t causeOr = 0;
    UIManager::UpdateProfileFrame profile{};
    if (fx.Manager().GetLastUpdateProfileFrame(profile))
        causeOr |= profile.GenAllPrimitivesRegenCause;
    for (int i = 0; i < 4; ++i)
    {
        fx.StepFrame();
        if (fx.Manager().GetLastUpdateProfileFrame(profile))
            causeOr |= profile.GenAllPrimitivesRegenCause;
    }

    EXPECT_TRUE(causeOr & 0x800u)
        << "the paint must come from the rangeless-content escalation, not a fallback regen";
    EXPECT_FALSE(fx.Primitives("status", UI::PrimitiveMode::Slug).empty())
        << "first text on an initially-empty label was never emitted";
}

TEST(UIRegressions, MeasureNodesClearStaleYogaChildrenBeforeSettingMeasureFunc)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto measured = std::make_unique<ToggleMeasuredElement>();
    ToggleMeasuredElement* measuredPtr = measured.get();

    // Frame 1: element has a child (Yoga node will have children), but measurement is disabled.
    measured->AddChild(std::make_unique<UIElement>());
    root->AddChild(std::move(measured));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_measure_children.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 300px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    // Frame 2: remove the child and enable measurement. The retained Yoga node still
    // has the previous children until we clear them before calling YGNodeSetMeasureFunc.
    ASSERT_EQ(measuredPtr->GetChildren().size(), 1u);
    UIElement* childPtr = measuredPtr->GetChildren()[0].get();
    ASSERT_NE(childPtr, nullptr);
    measuredPtr->RemoveChild(childPtr);
    EXPECT_TRUE(measuredPtr->GetChildren().empty());
    measuredPtr->SetMeasureEnabled(true);

    // Regression: previously this could crash with:
    // "Cannot set measure function: Nodes with measure functions cannot have children."
    ui.Update(0.0f, /*interactive=*/false);
}

TEST(UIRegressions, TreeChangedRebuildThenRelayoutDoesNotBreakInheritedFontSize)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    UIElement* rootPtr = root.get();
    root->SetId("root");
    // Mirror Editor styling: dockspace sets the base font size that children should inherit.
    root->AddClass("dockspace");

    // Two siblings so we can reorder them during a deferred action and change DOM/Yoga ordering.
    auto a = std::make_unique<UIElement>();
    UIElement* aPtr = a.get();
    a->SetId("a");

    auto b = std::make_unique<UIElement>();
    UIElement* bPtr = b.get();
    b->SetId("b");

    auto lbl = std::make_unique<Label>();
    lbl->SetId("lbl");
    lbl->SetText("Hello");
    b->AddChild(std::move(lbl));

    root->AddChild(std::move(a));
    root->AddChild(std::move(b));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Basic layout with inherited typography.
    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_treechanged_rel_layout_fontsize.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-direction: column; width: 200px; height: 200px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
#a, #b { width: 200px; height: 50px; }
#lbl { width: 200px; height: 50px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Frame 0: establish baseline.
    ui.Update(0.0f, /*interactive=*/false);
    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* lblEl0 = rootEl->FindById("lbl");
    ASSERT_NE(lblEl0, nullptr);
    EXPECT_NEAR(lblEl0->GetResolvedStyle().Visual.FontSize, 14.0f, 0.01f);

    // Clicking should trigger:
    // - a deferred action (treeChanged rebuild), and
    // - a relayout request in the same frame (ConsumeRelayoutRequest()).
    //
    // Regression: if the Yoga-node index map is stale after treeChanged rebuild,
    // parent indices can be wrong for a frame and inherited font-size can fall
    // back to the default (18px).
    bool didReorder = false;
    bPtr->RegisterEventHandler(kEventMouseDown, [rootPtr, aPtr, &didReorder](UIEvent& e)
                               {
                                   rootPtr->PostAction([rootPtr, aPtr, &didReorder]()
                                                       {
                                                           // Move 'a' to the end, swapping sibling order.
                                                           if (auto taken = rootPtr->TakeChild(aPtr))
                                                           {
                                                               rootPtr->AddChild(std::move(taken));
                                                               didReorder = true;
                                                           }
                                                           rootPtr->RequestRelayout();
                                                       });
                                   e.Stop();
                               });

    // Hover/click inside #b.
    ui.OnMouseMove(10.0f, 60.0f);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_TRUE(didReorder);

    UIElement* lblEl1 = rootEl->FindById("lbl");
    ASSERT_NE(lblEl1, nullptr);
    EXPECT_NEAR(lblEl1->GetResolvedStyle().Visual.FontSize, 14.0f, 0.01f);
}

TEST(UIRegressions, MultilineLabelHeightPreventsOverlapInScrollView)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dockspace");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    // Two stacked labels; the first is multiline and should reserve enough height
    // so the second does not overlap it.
    auto l1 = std::make_unique<Label>();
    l1->SetId("l1");
    l1->SetText("Line 1\nLine 2\nLine 3\nLine 4");
    svPtr->GetViewport()->AddChild(std::move(l1));

    auto l2 = std::make_unique<Label>();
    l2->SetId("l2");
    l2->SetText("After");
    svPtr->GetViewport()->AddChild(std::move(l2));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_multiline_label_no_overlap.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 240px; height: 160px; }
#sv { width: 240px; height: 160px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* l1El = rootEl->FindById("l1");
    UIElement* l2El = rootEl->FindById("l2");
    ASSERT_NE(l1El, nullptr);
    ASSERT_NE(l2El, nullptr);

    const float l1Bottom = l1El->GetLayoutY() + l1El->GetLayoutHeight();
    // Allow up to ~1px of layout overlap due to fractional Yoga rounding and
    // font metric differences across platforms/backends.
    EXPECT_GE(l2El->GetLayoutY() + 0.01f, l1Bottom - 1.0f);
}

TEST(UIRegressions, ScrollViewShowsHorizontalScrollbarForLongLabelLine)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dockspace");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    auto l = std::make_unique<Label>();
    l->SetId("long");
    l->SetText("This is a very very very very very very very very very very very very long line that should overflow.");
    svPtr->GetViewport()->AddChild(std::move(l));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_scrollview_hscroll_long_label.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 240px; height: 120px; }
#sv { width: 240px; height: 120px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    auto* hbar = dynamic_cast<Scrollbar*>(svPtr->GetHorizontalScrollbar());
    ASSERT_NE(hbar, nullptr);
    EXPECT_FALSE(hbar->IsHidden());
}

TEST(UIRegressions, ScrollViewShowsHorizontalScrollbarAfterTreeChangedRebuildSameFrame)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    UIElement* rootPtr = root.get();
    root->SetId("root");
    root->AddClass("dockspace");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    auto l = std::make_unique<Label>();
    Label* longLabel = l.get();
    l->SetId("long");
    l->SetText("Short");
    svPtr->GetViewport()->AddChild(std::move(l));

    // Mutate the label during event dispatch using a deferred action so UIManager
    // takes the treeChanged rebuild path within the same Update().
    bool sawClick = false;
    rootPtr->RegisterEventHandler(kEventMouseDown, [rootPtr, longLabel, &sawClick](UIEvent& e)
                                  {
                                      sawClick = true;
                                      rootPtr->PostAction([longLabel]()
                                                          {
                                                              longLabel->SetText("This is a very very very very very very very very very very very very long line that should overflow.");
                                                          });
                                      e.Stop();
                                  });

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_scrollview_hscroll_treechanged.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 240px; height: 120px; }
#sv { width: 240px; height: 120px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    // Initial layout.
    ui.Update(0.0f, /*interactive=*/false);

    // Click trigger to enqueue tree mutation.
    ui.OnMouseMove(1.0f, 1.0f);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_TRUE(sawClick) << "MouseDown did not reach root handler (event dispatch/hit-testing issue)";
    EXPECT_NE(longLabel->GetText().find("very very"), std::string::npos) << "Deferred PostAction did not run (label text unchanged)";

    auto* hbar = dynamic_cast<Scrollbar*>(svPtr->GetHorizontalScrollbar());
    ASSERT_NE(hbar, nullptr);
    const float vw = svPtr->GetViewportWidth();
    const float cw = svPtr->GetContentWidth();
    EXPECT_GT(vw, 0.0f) << "ScrollView viewport width not set; contentW=" << cw;
    EXPECT_GT(cw, vw + 0.5f) << "ScrollView content width did not exceed viewport; viewportW=" << vw << " contentW=" << cw;
    EXPECT_FALSE(hbar->IsHidden()) << "Expected hscroll visible; viewportW=" << vw << " contentW=" << cw;
}

TEST(UIRegressions, ScrollViewHorizontalScrollbarDoesNotOverlapViewport)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root"); // matches Editor theme selector

    auto panel = std::make_unique<UIElement>();
    panel->SetId("panel");
    panel->AddClass("panel");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    panel->AddChild(std::move(sv));

    root->AddChild(std::move(panel));

    // A long, multiline label to match ScriptInspector's large source block.
    auto l = std::make_unique<Label>();
    l->SetId("long");
    l->SetText("line0\nThis is a very very very very very very very very very very very very long line that should overflow.\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\nline11\nline12\nline13\nline14\nline15\nline16\nline17\nline18\nline19");
    svPtr->GetViewport()->AddChild(std::move(l));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Attach the Editor theme plus a small override to constrain root size.
    // The theme is read from the copy the build stages next to the test exe, at
    // the same relative path the editor reads it from under its own exe; its
    // @imports resolve relative to that staged file.
    const auto theme = PathUtils::GetExecutableDirectory() / "Assets" / "UI" / "theme.css";
    ASSERT_TRUE(std::filesystem::exists(theme)) << "Editor theme not staged at " << theme.string();
    ASSERT_TRUE(ui.AttachStyleFromFile(theme.string()));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_scrollview_no_overlap.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 320px; height: 240px; }
#panel { flex-grow: 1; min-width: 0; min-height: 0; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    auto* hbar = dynamic_cast<Scrollbar*>(svPtr->GetHorizontalScrollbar());
    ASSERT_NE(hbar, nullptr);
    ASSERT_FALSE(hbar->IsHidden());

    // Internal structure:
    // sv
    //  ├── row
    //  │    ├── clipViewport
    //  │    │    └── scrollContent (svPtr->GetViewport())
    //  │    └── vbar
    //  └── hbar
    ASSERT_GE(svPtr->GetChildren().size(), 2u);
    UIElement* row = svPtr->GetChildren()[0].get();
    UIElement* hbarEl = svPtr->GetChildren()[1].get();
    ASSERT_NE(row, nullptr);
    ASSERT_NE(hbarEl, nullptr);
    ASSERT_GE(row->GetChildren().size(), 1u);
    UIElement* clipViewport = row->GetChildren()[0].get();
    ASSERT_NE(clipViewport, nullptr);

    const float viewportBottom = clipViewport->GetLayoutY() + clipViewport->GetLayoutHeight();
    const float hbarTop = hbarEl->GetLayoutY();
    EXPECT_LE(viewportBottom, hbarTop + 0.5f) << "scroll-viewport overlaps horizontal scrollbar";

    const float svBottom = svPtr->GetLayoutY() + svPtr->GetLayoutHeight();
    const float hbarBottom = hbarEl->GetLayoutY() + hbarEl->GetLayoutHeight();
    EXPECT_LE(hbarBottom, svBottom + 0.5f) << "horizontal scrollbar extends outside ScrollView bounds";
}

TEST(UIRegressions, VerticalScrollbarVisibilityReflowsPercentageWidthDescendants)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    struct PropertyRow
    {
        UIElement* Row = nullptr;
        UIElement* Label = nullptr;
        UIElement* Field = nullptr;
    };

    auto buildContent = [svPtr](bool tall) {
        svPtr->GetViewport()->RemoveAllChildren();

        auto section = std::make_unique<UIElement>();
        section->AddClass("section");

        auto body = std::make_unique<UIElement>();
        body->AddClass("body");

        auto row = std::make_unique<UIElement>();
        row->AddClass("property-row");

        auto label = std::make_unique<UIElement>();
        label->AddClass("label-cell");

        auto field = std::make_unique<UIElement>();
        field->AddClass("field-cell");

        PropertyRow result{row.get(), label.get(), field.get()};
        row->AddChild(std::move(label));
        row->AddChild(std::move(field));
        body->AddChild(std::move(row));

        auto filler = std::make_unique<UIElement>();
        filler->AddClass(tall ? "tall-filler" : "short-filler");
        body->AddChild(std::move(filler));

        section->AddChild(std::move(body));
        svPtr->GetViewport()->AddChild(std::move(section));
        return result;
    };

    PropertyRow tallRow = buildContent(true);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() /
                     "ui_regression_scrollbar_visibility_percent_reflow.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 511px; height: 300px; }
#sv { width: 511px; height: 300px; }
.scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
.scroll-viewport { display: flex; flex-direction: column; flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
.scroll-content { display: flex; flex-direction: column; padding: 8px; min-width: 0; }
.scrollbar.vertical { margin-right: 6px; }
.section { display: flex; flex-direction: column; width: 100%; }
.body { display: flex; flex-direction: column; padding-left: 28px; padding-right: 8px; }
.property-row { display: flex; flex-direction: row; width: 100%; gap: 8px; }
.label-cell { flex: 0 0 37%; width: 37%; min-width: 0; max-width: 37%; height: 27px; }
.field-cell { flex: 1 1 0%; min-width: 0; height: 27px; }
.tall-filler { height: 360px; min-height: 360px; flex-shrink: 0; }
.short-filler { height: 40px; min-height: 40px; flex-shrink: 0; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    ui.Update(0.0f, /*interactive=*/false);

    auto* vbar = dynamic_cast<Scrollbar*>(svPtr->GetVerticalScrollbar());
    ASSERT_NE(vbar, nullptr);
    ASSERT_FALSE(vbar->IsHidden());
    EXPECT_NEAR(svPtr->GetViewportWidth(), 493.0f, 0.5f);
    EXPECT_NEAR(tallRow.Row->GetLayoutWidth(), 441.0f, 0.5f);
    EXPECT_NEAR(tallRow.Label->GetLayoutWidth(), 163.0f, 0.5f);

    PropertyRow shortRow = buildContent(false);
    ui.Update(0.0f, /*interactive=*/false);

    ASSERT_TRUE(vbar->IsHidden());
    EXPECT_NEAR(svPtr->GetViewportWidth(), 511.0f, 0.5f);
    EXPECT_NEAR(shortRow.Row->GetLayoutWidth(), 459.0f, 0.5f);
    EXPECT_NEAR(shortRow.Label->GetLayoutWidth(), 170.0f, 0.5f);
    EXPECT_NEAR(shortRow.Field->GetLayoutWidth(), 281.0f, 0.5f);

    PropertyRow tallAgainRow = buildContent(true);
    ui.Update(0.0f, /*interactive=*/false);

    ASSERT_FALSE(vbar->IsHidden());
    EXPECT_NEAR(svPtr->GetViewportWidth(), 493.0f, 0.5f);
    EXPECT_NEAR(tallAgainRow.Row->GetLayoutWidth(), 441.0f, 0.5f);
    EXPECT_NEAR(tallAgainRow.Label->GetLayoutWidth(), 163.0f, 0.5f);
    EXPECT_NEAR(tallAgainRow.Field->GetLayoutWidth(), 270.0f, 0.5f);
}

TEST(UIRegressions, ScrollbarDragDoesNotResetScrollX)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dockspace");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    auto l = std::make_unique<Label>();
    l->SetId("long");
    l->SetText("This is a very very very very very very very very very very very very long line that should overflow.");
    svPtr->GetViewport()->AddChild(std::move(l));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_scrollbar_drag_persists_scroll.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 240px; height: 120px; }
#sv { width: 240px; height: 120px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    auto* hbar = dynamic_cast<Scrollbar*>(svPtr->GetHorizontalScrollbar());
    ASSERT_NE(hbar, nullptr);
    ASSERT_FALSE(hbar->IsHidden());
    EXPECT_NEAR(svPtr->GetScrollX(), 0.0f, 0.01f);

    // Drag across the horizontal scrollbar.
    const float startX = hbar->GetLayoutX() + hbar->GetLayoutWidth() * 0.25f;
    const float endX = hbar->GetLayoutX() + hbar->GetLayoutWidth() * 0.75f;
    const float y = hbar->GetLayoutY() + hbar->GetLayoutHeight() * 0.5f;

    ui.OnMouseMove(startX, y);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);

    ui.OnMouseMove(endX, y);
    ui.Update(0.0f, /*interactive=*/true);

    const float scDragged = svPtr->GetScrollX();
    EXPECT_GT(scDragged, 0.5f);

    ui.OnMouseButton(0, false);
    ui.Update(0.0f, /*interactive=*/true);

    // Must not snap back to origin on release.
    EXPECT_NEAR(svPtr->GetScrollX(), scDragged, 0.5f);

    // Must remain stable next frame as well.
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_NEAR(svPtr->GetScrollX(), scDragged, 0.5f);
}

TEST(UIRegressions, ScrollbarDragDoesNotResetScrollY)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dockspace");

    auto sv = std::make_unique<ScrollView>();
    ScrollView* svPtr = sv.get();
    sv->SetId("sv");
    root->AddChild(std::move(sv));

    // Add enough lines to require vertical scrolling.
    for (int i = 0; i < 40; ++i)
    {
        auto l = std::make_unique<Label>();
        l->SetText(std::string("Line ") + std::to_string(i) + " - Some text to make this label non-empty.");
        svPtr->GetViewport()->AddChild(std::move(l));
    }

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_scrollbar_drag_persists_scroll_y.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 240px; height: 120px; }
#sv { width: 240px; height: 120px; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    auto* vbar = dynamic_cast<Scrollbar*>(svPtr->GetVerticalScrollbar());
    ASSERT_NE(vbar, nullptr);
    ASSERT_FALSE(vbar->IsHidden());
    EXPECT_NEAR(svPtr->GetScrollY(), 0.0f, 0.01f);

    // Drag down across the vertical scrollbar.
    const float x = vbar->GetLayoutX() + vbar->GetLayoutWidth() * 0.5f;
    const float startY = vbar->GetLayoutY() + vbar->GetLayoutHeight() * 0.25f;
    const float endY = vbar->GetLayoutY() + vbar->GetLayoutHeight() * 0.75f;

    ui.OnMouseMove(x, startY);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);

    ui.OnMouseMove(x, endY);
    ui.Update(0.0f, /*interactive=*/true);

    const float scDragged = svPtr->GetScrollY();
    EXPECT_GT(scDragged, 0.5f);

    ui.OnMouseButton(0, false);
    ui.Update(0.0f, /*interactive=*/true);

    // Must not snap back to origin on release.
    EXPECT_NEAR(svPtr->GetScrollY(), scDragged, 0.5f);

    // Must remain stable next frame as well.
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_NEAR(svPtr->GetScrollY(), scDragged, 0.5f);
}

TEST(UIRegressions, WordBreakBreakAllWrapsLabelHeight)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dockspace");

    const std::string longWord(80, 'W');

    auto nowrap = std::make_unique<Label>();
    nowrap->SetId("nowrap");
    nowrap->SetText(longWord);
    root->AddChild(std::move(nowrap));

    auto wrap = std::make_unique<Label>();
    wrap->SetId("wrap");
    wrap->SetText(longWord);
    root->AddChild(std::move(wrap));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_word_break_wraps_label.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-direction: column; width: 180px; height: 200px; }
#nowrap { width: 120px; }
#wrap { width: 120px; word-break: break-all; }
.dockspace { font-size: 14px; font-family: "Segoe UI", Arial; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* nowrapEl = rootEl->FindById("nowrap");
    UIElement* wrapEl = rootEl->FindById("wrap");
    ASSERT_NE(nowrapEl, nullptr);
    ASSERT_NE(wrapEl, nullptr);
    EXPECT_EQ(wrapEl->GetResolvedStyle().Visual.WordBreak, WordBreak::BreakAll);

    const float nowrapH = nowrapEl->GetLayoutHeight();
    const float wrapH = wrapEl->GetLayoutHeight();
    EXPECT_GT(wrapH, nowrapH * 1.5f) << "word-break: break-all did not increase label height";
}

// A control hides itself (display:none via a class) and marks ONLY its own subtree dirty -- no
// parent poke, no RequestRelayout -- which is the simplified InspectorSection / Foldout collapse
// pattern (the parent poke + RequestRelayout were dropped once UIManager::Update's deferred relayout
// was fixed to carry the frame-start dirty). This asserts the sibling below reflows with that
// minimal marking. NOTE: the headless test harness always runs the layout-signature pass, so this
// guards the minimal-marks reflow *behaviour*; the deferred-dirty gate that was the actual bug only
// reproduces in the live editor and is verified there.
TEST(UIRegressions, DeferredRelayoutReflowsSiblingAfterDisplayNoneCollapse)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto a = std::make_unique<UIElement>();
    a->SetId("a");
    UIElement* aPtr = a.get();
    root->AddChild(std::move(a));
    auto b = std::make_unique<UIElement>();
    b->SetId("b");
    root->AddChild(std::move(b));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_deferred_relayout_collapse.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-direction: column; width: 100px; height: 200px; }
#a { height: 40px; }
#b { height: 20px; }
.hidden { display: none; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* bEl = rootEl->FindById("b");
    ASSERT_NE(bEl, nullptr);
    const float bYBefore = bEl->GetLayoutY();
    EXPECT_GT(bYBefore, 30.0f) << "precondition: b should sit below a (~40px)";

    // Collapse 'a' AFTER the initial solve, exactly the way InspectorSection/Foldout collapse do.
    aPtr->AddClass("hidden");
    aPtr->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);

    ui.Update(0.0f, /*interactive=*/false);

    const float bYAfter = bEl->GetLayoutY();
    EXPECT_LT(bYAfter, 5.0f) << "sibling did not reflow after display:none collapse (bYBefore="
                             << bYBefore << " bYAfter=" << bYAfter
                             << ") -- deferred relayout dropped the frame-start dirty";
}

// Same minimal-marks reflow for a self-RESIZE (the CurvePresetPicker case): a control grows its own
// height via a Style::Height override and marks only itself LayoutDirty (no parent poke, no
// RequestRelayout). The sibling below must move down on the next frame.
TEST(UIRegressions, DeferredRelayoutReflowsSiblingAfterSelfResize)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto a = std::make_unique<UIElement>();
    a->SetId("a");
    UIElement* aPtr = a.get();
    root->AddChild(std::move(a));
    auto b = std::make_unique<UIElement>();
    b->SetId("b");
    root->AddChild(std::move(b));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_deferred_relayout_resize.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; flex-direction: column; width: 100px; height: 300px; }
#a { height: 40px; }
#b { height: 20px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* bEl = rootEl->FindById("b");
    ASSERT_NE(bEl, nullptr);
    const float bYBefore = bEl->GetLayoutY();

    // Grow 'a' AFTER the initial solve via a direct Style::Height override (the CurvePresetPicker
    // path); mark only 'a' LayoutDirty -- no parent poke, no RequestRelayout.
    aPtr->Overrides().Set(Style::Height, StyleLength::Px(120.0f));
    aPtr->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);

    ui.Update(0.0f, /*interactive=*/false);

    const float bYAfter = bEl->GetLayoutY();
    EXPECT_GT(bYAfter, bYBefore + 50.0f) << "sibling did not move down after self-resize (bYBefore="
                                         << bYBefore << " bYAfter=" << bYAfter << ")";
}



// Repro for the project-picker search bar: the absolutely-positioned clear
// button (z-index 20) sits on top of the TextField (inline z-index 10, width
// 100%). Clicking the X must reach the Button and run its OnClick, clearing
// the field — not get swallowed by the field's text-editing pointer routing.
// Mirrors BuildPanelSearchBar wiring + ProjectFolderPickerModal's inline
// overrides and root mouse-down focus handler.
TEST(UIRegressions, SearchBarClearButtonClickClearsFieldOverText)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto searchBar = std::make_unique<UIElement>();
    searchBar->SetId("project-search");
    searchBar->AddClass("panel-search-bar");
    UIElement* searchBarPtr = searchBar.get();

    auto field = std::make_unique<TextField>();
    TextField* fieldPtr = field.get();
    field->SetId("search-field");
    field->AddClass("panel-search-field");
    field->SetValue("");

    auto clearButton = std::make_unique<Button>();
    clearButton->SetId("search-clear");
    clearButton->AddClass("panel-search-clear");
    clearButton->AddClass("xclose-icon");
    Button* clearPtr = clearButton.get();
    searchBar->AddChild(std::move(field));

    auto icon = std::make_unique<UIElement>();
    icon->AddClass("panel-search-icon");
    searchBar->AddChild(std::move(icon));
    searchBar->AddChild(std::move(clearButton));

    // Shared PanelSearchBar clear handler shape.
    int clearClicks = 0;
    clearPtr->RegisterEventHandler(kEventButtonClick, [fieldPtr, &clearClicks](UIEvent&) {
        ++clearClicks;
        if (fieldPtr->GetValue().empty())
            return;
        fieldPtr->SetValue("");
    });

    // Track who receives mouse-down for diagnostics.
    int fieldMouseDown = 0;
    int clearMouseDown = 0;
    fieldPtr->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++fieldMouseDown; });
    clearPtr->RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++clearMouseDown; });

    // Picker's inline overrides.
    fieldPtr->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::Position, PositionType::Relative)
        // The fix mirrored in ProjectFolderPickerModal: interactive elements
        // inside a high-z modal context must set z-index above that context,
        // not the small local values from the panel-search CSS (10/20) —
        // those lose the flat hit test to every plain container in the modal.
        .Set(Style::ZIndex, 10002)
        .Set(Style::PointerEvents, true);
    clearPtr->SetFocusProxy(fieldPtr);
    clearPtr->Overrides().Set(Style::ZIndex, 10003);

    // Picker's root mouse-down focus handler.
    searchBarPtr->RegisterEventHandler(kEventMouseDown, [&, fieldPtr](UIEvent& e) {
        if (e.Button == 0 || e.Button == 1)
        {
            if (UIManager* manager = fieldPtr->GetOwnerManager())
                manager->FocusElement(fieldPtr);
        }
    });

    // Modal wrapper mirroring ProjectFolderPickerModal: root z-index 10000,
    // window z-index 10001. Hit-testing compares z flat across stacking
    // contexts, so a small explicit z-index on a descendant loses to the
    // modal context.
    auto modal = std::make_unique<UIElement>();
    modal->SetId("modal");
    modal->Overrides().Set(Style::ZIndex, 10000);
    auto window = std::make_unique<UIElement>();
    window->SetId("modal-window");
    window->Overrides().Set(Style::ZIndex, 10001);
    window->AddChild(std::move(searchBar));
    modal->AddChild(std::move(window));
    root->AddChild(std::move(modal));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_regression_search_clear.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 400px; height: 100px; }
#modal { display: flex; width: 400px; height: 100px; }
#modal-window { display: flex; width: 380px; height: 60px; }
#project-search { display: flex; flex-direction: row; position: relative; width: 320px; height: 29px; }
.panel-search-field { width: 100%; height: 100%; padding-right: 32px; position: relative; z-index: 10; }
.panel-search-field TextInput { background-color: transparent; padding: 4px 24px 4px 24px; height: 100%; width: 100%; }
.panel-search-icon { position: absolute; right: 6px; top: 50%; margin-top: -6px; width: 12px; height: 12px; pointer-events: none; z-index: 1; }
.panel-search-clear { position: absolute; left: 4px; top: 50%; margin-top: -8px; width: 16px; height: 16px; min-width: 16px; min-height: 16px; padding: 0; z-index: 20; }
.hidden { display: none; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);

    // Click into the field and type, exactly like a user: the field gains
    // focus and the TextInput has an active editing session when the X is
    // later clicked.
    {
        const float fx = fieldPtr->GetLayoutX() + fieldPtr->GetLayoutWidth() * 0.5f;
        const float fy = fieldPtr->GetLayoutY() + fieldPtr->GetLayoutHeight() * 0.5f;
        ui.OnMouseMove(fx, fy);
        ui.Update(0.0f, /*interactive=*/true);
        ui.OnMouseButton(0, true);
        ui.Update(0.0f, /*interactive=*/true);
        ui.OnMouseButton(0, false);
        ui.Update(0.0f, /*interactive=*/true);
    }
    for (char c : std::string("fgdfgdfggfd"))
        ui.OnChar((unsigned)c);
    ui.Update(0.0f, /*interactive=*/true);

    ASSERT_EQ(fieldPtr->GetValue(), "fgdfgdfggfd") << "typing did not reach the field";

    // Sanity: the clear button got real layout at the bar's left edge.
    const float cx = clearPtr->GetLayoutX() + clearPtr->GetLayoutWidth() * 0.5f;
    const float cy = clearPtr->GetLayoutY() + clearPtr->GetLayoutHeight() * 0.5f;
    ASSERT_GT(clearPtr->GetLayoutWidth(), 0.0f);
    ASSERT_GT(clearPtr->GetLayoutHeight(), 0.0f);

    // Click the X.
    ui.OnMouseMove(cx, cy);
    ui.Update(0.0f, /*interactive=*/true);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);
    ui.OnMouseButton(0, false);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(clearMouseDown, 1) << "clear button did not receive mouse-down (fieldMouseDown="
                                 << fieldMouseDown << ")";
    EXPECT_EQ(clearClicks, 1) << "clear button OnClick did not fire";
    EXPECT_EQ(fieldPtr->GetValue(), "") << "field text was not cleared by the clear button";
}

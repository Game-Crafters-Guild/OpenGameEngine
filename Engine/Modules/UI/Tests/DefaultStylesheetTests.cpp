// The UI module's default stylesheet (Assets/defaults.css, compiled into the module), which every
// UIManager loads first. A game document ships only its own stylesheet, so without these defaults
// a slider it does not style draws no track and no thumb, a toggle no knob and a checkbox no box.
// These tests build a bare UIManager with no document CSS and check that each of those parts can
// be seen, and that a document's own rule still wins over the default.

#include <gtest/gtest.h>

#include "DefaultStylesheet.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr uint32_t kViewportW = 400;
constexpr uint32_t kViewportH = 300;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr float kControlWidthPx = 200.0f;
constexpr float kControlHeightPx = 24.0f;

bool IsVisible(uint32_t argb)
{
    return (argb >> 24) != 0;
}

const StringId kTrackFillVar = HashStringId("--slider-track-fill");
const StringId kTrackVar = HashStringId("--slider-track");

uint32_t TrackFill(const Slider& slider)
{
    return slider.GetResolvedStyle().GetCustomColor(kTrackFillVar).value_or(0u);
}

// The fill colours of the primitives the slider paints for its track and ticks.
std::vector<uint32_t> PaintedTrackColors(Slider& slider)
{
    std::vector<UI::UIPrimitive> primitives;
    UI::PrimitiveEmitContext ctx{primitives, /*ClipIndex=*/0, /*Opacity=*/1.0f};
    slider.OnGeneratePrimitives(ctx, slider.GetResolvedStyle(), 0.0f, 0.0f,
                                slider.GetLayoutWidth(), slider.GetLayoutHeight());
    std::vector<uint32_t> colors;
    for (const UI::UIPrimitive& primitive : primitives)
        colors.push_back(primitive.FillColor);
    return colors;
}

UIElement* FindByClass(UIElement* root, const std::string& className)
{
    if (!root)
        return nullptr;
    if (root->HasClass(className) && !root->HasClass("hidden"))
        return root;
    for (const auto& child : root->GetChildren())
    {
        if (UIElement* found = FindByClass(child.get(), className))
            return found;
    }
    return nullptr;
}

// A bare UIManager: no theme, no document stylesheet, only what the UI module loads itself.
struct BareManagerFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Device = nullptr;
    std::unique_ptr<UIManager> Ui;
    UIElement* Document = nullptr;

    bool Build(std::unique_ptr<UIElement> control)
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Ui = std::make_unique<UIManager>(Device);
        Ui->SetLayoutSizeOverride(kViewportW, kViewportH);

        control->Overrides()
            .Set(Style::Width, StyleLength::Px(kControlWidthPx))
            .Set(Style::Height, StyleLength::Px(kControlHeightPx));
        auto document = std::make_unique<UIElement>();
        document->AddChild(std::move(control));
        Document = document.get();
        Ui->SetRoot(std::move(document));
        Settle();
        return true;
    }

    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
    }
};

} // namespace

// The sheet is compiled in, so it exists in every process. The loader's checks are asserts, off in
// a shipped game: an unsupported declaration would be dropped there silently, and an !important
// one would outrank author rules, because the cascade does not reverse origins for !important.
TEST(DefaultStylesheet, TheCompiledInSheetParsesWithEveryDeclarationSupported)
{
    const StylesheetHandle sheet = UI::GetDefaultStylesheet();
    ASSERT_NE(sheet, nullptr);
    EXPECT_FALSE(sheet->Rules.empty());
    EXPECT_TRUE(sheet->UnknownProperties.empty())
        << "first unsupported property: " << sheet->UnknownProperties.front();
    EXPECT_EQ(sheet->Origin, StyleOrigin::UserAgent);
    for (const CSSRule& rule : sheet->Rules)
    {
        for (const StyleProperty& property : rule.Properties)
            EXPECT_FALSE(property.Important) << "a user-agent sheet may not use !important";
        // An unimplemented pseudo-class parses as a custom state, which no engine control sets:
        // in this sheet it is a typo, and the rule would never match.
        for (const SelectorTerm& term : rule.Selector.Terms)
        {
            for (const PseudoClass& pseudo : term.Selector.Pseudos)
                EXPECT_NE(pseudo.PseudoKind, PseudoClass::Kind::Custom) << "unknown pseudo-class :" << pseudo.CustomName;
        }
    }
}

TEST(DefaultStylesheet, ABareManagerGivesASliderAVisibleTrackAndThumb)
{
    auto owned = std::make_unique<Slider>();
    Slider* slider = owned.get();
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";

    const ResolvedStyle& style = slider->GetResolvedStyle();
    EXPECT_TRUE(IsVisible(style.GetCustomColor(kTrackFillVar).value_or(0u)))
        << "the filled part of the track is transparent";
    EXPECT_TRUE(IsVisible(style.GetCustomColor(kTrackVar).value_or(0u))) << "the rest of the rail is transparent";

    UIElement* thumb = FindByClass(slider, "slider-thumb");
    ASSERT_NE(thumb, nullptr);
    EXPECT_TRUE(IsVisible(thumb->GetResolvedStyle().Visual.BackgroundColor)) << "the thumb is transparent";
    EXPECT_GT(thumb->GetLayoutWidth(), 0.0f) << "the thumb has no size";
    EXPECT_GT(thumb->GetLayoutHeight(), 0.0f) << "the thumb has no size";

    // Outside range mode the slider hides its second thumb with the `hidden` class, which only
    // a stylesheet turns into display: none. Without it a bare slider shows two thumbs.
    int displayedThumbs = 0;
    for (const auto& child : slider->GetChildren())
    {
        if (child->HasClass("slider-thumb") &&
            child->GetResolvedStyle().Layout.DisplayMode != DisplayMode::None)
            ++displayedThumbs;
    }
    EXPECT_EQ(displayedThumbs, 1) << "the hidden second thumb is still displayed";
}

// The default sheet moves a checked toggle's knob to the far end: flush to the start, less its
// margin, when unchecked, and flush to the end when checked.
TEST(DefaultStylesheet, ACheckedTogglesKnobSitsAtTheEnd)
{
    constexpr float kKnobMarginPx = 2.0f;
    constexpr float kTolerancePx = 0.5f;
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";
    UIElement* knob = FindByClass(toggle, "toggle-knob");
    ASSERT_NE(knob, nullptr);
    ASSERT_GT(knob->GetLayoutWidth(), 0.0f);
    const float start = knob->GetLayoutX() - toggle->GetLayoutX();
    EXPECT_NEAR(start, kKnobMarginPx, kTolerancePx) << "the unchecked knob is not at the start";

    toggle->SetChecked(true);
    fixture.Settle();
    const float endGap =
        (toggle->GetLayoutX() + toggle->GetLayoutWidth()) - (knob->GetLayoutX() + knob->GetLayoutWidth());
    EXPECT_NEAR(endGap, kKnobMarginPx, kTolerancePx) << "the checked knob is not at the end";
}

TEST(DefaultStylesheet, ABareManagerGivesAToggleAKnobAndACheckboxABox)
{
    auto root = std::make_unique<UIElement>();
    auto toggle = std::make_unique<Toggle>();
    auto checkbox = std::make_unique<Checkbox>();
    Toggle* toggleRaw = toggle.get();
    Checkbox* checkboxRaw = checkbox.get();
    root->AddChild(std::move(toggle));
    root->AddChild(std::move(checkbox));
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << "no Vulkan device";

    UIElement* knob = FindByClass(toggleRaw, "toggle-knob");
    ASSERT_NE(knob, nullptr);
    EXPECT_TRUE(IsVisible(knob->GetResolvedStyle().Visual.BackgroundColor)) << "the toggle knob is transparent";
    EXPECT_GT(knob->GetLayoutWidth(), 0.0f) << "the toggle knob has no size";

    UIElement* box = FindByClass(checkboxRaw, "checkbox-box");
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(IsVisible(box->GetResolvedStyle().Visual.BackgroundColor)) << "the checkbox box is transparent";
    EXPECT_GT(box->GetLayoutWidth(), 0.0f) << "the checkbox box has no size";
}

// A game document recolours a slider's track through the track properties, here pointing one at
// its own token the way a theme does; the slider paints what the property resolves to.
TEST(DefaultStylesheet, ADocumentRecoloursTheSliderTrackThroughItsProperties)
{
    constexpr uint32_t kDocumentTrackFill = 0xFFFF0000u;
    auto owned = std::make_unique<Slider>();
    Slider* slider = owned.get();
    slider->AddClass("probe-slider");
    slider->SetValue(slider->GetMax());
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";

    ASSERT_NE(TrackFill(*slider), kDocumentTrackFill)
        << "the default already uses the document's colour; this test is vacuous";

    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        ":root { --game-accent: #FF0000; } .probe-slider { --slider-track-fill: var(--game-accent); }", sheet));
    fixture.Document->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
    fixture.Settle();

    EXPECT_EQ(TrackFill(*slider), kDocumentTrackFill);
    EXPECT_TRUE(IsVisible(slider->GetResolvedStyle().GetCustomColor(kTrackVar).value_or(0u)))
        << "the document set only the filled colour, so the rail keeps the default";

    const std::vector<uint32_t> painted = PaintedTrackColors(*slider);
    EXPECT_NE(std::find(painted.begin(), painted.end(), UI::PackFromARGB(kDocumentTrackFill)), painted.end())
        << "the slider does not paint its filled track in the property's colour";
}

// The default sheet is the user-agent origin of the cascade: every author rule beats every default
// rule before specificity or sheet order is compared, the way a page's CSS beats a browser's
// built-in sheet. The tests below each set a property the default sheet also sets.

namespace
{

StylesheetHandle ParseSheet(const char* css)
{
    auto sheet = std::make_shared<Stylesheet>();
    EXPECT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, *sheet)) << css;
    return sheet;
}

} // namespace

// A theme added to the manager after construction, at the default's own specificity.
TEST(DefaultStylesheet, AGlobalThemeRuleAddedLaterWinsOverTheDefault)
{
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";
    UIElement* knob = FindByClass(toggle, "toggle-knob");
    ASSERT_NE(knob, nullptr);
    ASSERT_NE(knob->GetResolvedStyle().Visual.BackgroundColor, 0xFF00FF00u)
        << "the default already uses the theme's colour; this test is vacuous";

    fixture.Ui->AddStylesheet(ParseSheet(".toggle-knob { background-color: #00FF00; }"));
    fixture.Ui->MarkStyleDirtyAll();
    fixture.Settle();
    EXPECT_EQ(knob->GetResolvedStyle().Visual.BackgroundColor, 0xFF00FF00u);
}

// A theme's own class on an element the control also marks `hidden`.
TEST(DefaultStylesheet, AGlobalThemeDisplayRuleWinsOverTheDefaultHidden)
{
    auto owned = std::make_unique<UIElement>();
    UIElement* element = owned.get();
    element->AddClass("probe-box");
    element->AddClass("hidden");
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_EQ(element->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None)
        << "the default .hidden rule does not apply; this test is vacuous";

    fixture.Ui->AddStylesheet(ParseSheet(".probe-box { display: flex; }"));
    fixture.Ui->MarkStyleDirtyAll();
    fixture.Settle();
    EXPECT_EQ(element->GetResolvedStyle().Layout.DisplayMode, DisplayMode::Flex);
}

// A game document's type selector (0,0,1) against the default's `.toggle` class rule (0,1,0).
TEST(DefaultStylesheet, ADocumentTypeSelectorWinsOverTheDefaultClassRule)
{
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_EQ(toggle->GetResolvedStyle().Layout.JustifyContent, JustifyContent::FlexStart)
        << "the default .toggle rule does not apply; this test is vacuous";

    fixture.Document->AddStylesheet(ParseSheet("toggle { border-left-color: #0000FF; justify-content: center; }"));
    fixture.Settle();
    ASSERT_EQ(toggle->GetResolvedStyle().Visual.BorderColor.Left, 0xFF0000FFu)
        << "the type selector does not match; this test is vacuous";
    EXPECT_EQ(toggle->GetResolvedStyle().Layout.JustifyContent, JustifyContent::Center);
}

// A game document's single class against the default's `.toggle:checked` (0,2,0).
TEST(DefaultStylesheet, ADocumentClassRuleWinsOverTheDefaultCheckedToggle)
{
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    toggle->AddClass("probe-toggle");
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";

    fixture.Document->AddStylesheet(ParseSheet(".probe-toggle { justify-content: center; }"));
    fixture.Settle();
    ASSERT_EQ(toggle->GetResolvedStyle().Layout.JustifyContent, JustifyContent::Center)
        << "the unchecked toggle does not take the document rule; this test is vacuous";

    toggle->SetChecked(true);
    fixture.Settle();
    EXPECT_EQ(toggle->GetResolvedStyle().Layout.JustifyContent, JustifyContent::Center);
}

// The origin, not the load order, puts the defaults under a theme: a default sheet that ends up
// after the theme in the manager's list still loses to it.
TEST(DefaultStylesheet, TheDefaultLosesToAThemeEvenWhenLoadedAfterIt)
{
    auto owned = std::make_unique<Toggle>();
    Toggle* toggle = owned.get();
    BareManagerFixture fixture;
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << "no Vulkan device";
    UIElement* knob = FindByClass(toggle, "toggle-knob");
    ASSERT_NE(knob, nullptr);

    const StylesheetHandle defaults = UI::GetDefaultStylesheet();
    ASSERT_NE(defaults, nullptr);
    const StylesheetHandle theme = ParseSheet(".toggle-knob { background-color: #00FF00; }");
    fixture.Ui->ReplaceGlobalStylesheetBlock({defaults.get()}, {theme, defaults});
    ASSERT_EQ(fixture.Ui->GetStylesheets().size(), 2u);
    ASSERT_EQ(fixture.Ui->GetStylesheets().back(), defaults) << "the defaults are not last; this test is vacuous";

    fixture.Ui->MarkStyleDirtyAll();
    fixture.Settle();
    EXPECT_EQ(knob->GetResolvedStyle().Visual.BackgroundColor, 0xFF00FF00u);
}

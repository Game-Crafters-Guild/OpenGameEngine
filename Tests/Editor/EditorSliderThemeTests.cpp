// The editor theme owns its slider track colours. The UI module's default stylesheet gives every
// slider a track, but the editor's look must not depend on it: a change to the game default would
// otherwise recolour every editor slider. These tests lay sliders out over the REAL editor theme
// sheets and check that the track follows the editor's own tokens.

#include <gtest/gtest.h>

#include "UI/Controls/ItemSizeSlider.h"
#include "UI/Controls/Slider.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using GameEngine::Slider;
using GameEngine::StringId;
using GameEngine::Stylesheet;
using GameEngine::UIElement;
using GameEngine::UIManager;

namespace
{

constexpr uint32_t kViewportW = 400;
constexpr uint32_t kViewportH = 300;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;

const StringId kTrackFillVar = GameEngine::HashStringId("--slider-track-fill");
const StringId kTrackVar = GameEngine::HashStringId("--slider-track");
const StringId kTickVar = GameEngine::HashStringId("--slider-tick");

// The theme sheets that colour a slider, in theme.css import order, then the size slider's own.
constexpr const char* kSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/slider.css",
    "Assets/UI/controls/ItemSizeSlider/ItemSizeSlider.css",
};

std::string ReadEditorFile(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

uint32_t CustomColor(const UIElement& element, StringId var)
{
    return element.GetResolvedStyle().GetCustomColor(var).value_or(0u);
}

// A real UIManager over the editor's slider sheets, plus any sheet the test appends after them.
struct SliderThemeFixture
{
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<UIManager> Ui;
    Slider* Plain = nullptr;
    GameEngine::EditorUI::ItemSizeSlider* SizeSlider = nullptr;
    UIElement* Root = nullptr;

    // False only without a Vulkan device; an editor sheet that does not read or parse fails the test.
    bool Build(const std::string& appendedCss = {})
    {
        Device = MakeHeadlessDevice();
        if (!Device)
            return false;
        Ui = std::make_unique<UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(kViewportW, kViewportH);

        std::vector<std::string> sources;
        for (const char* sheetPath : kSheets)
        {
            sources.push_back(ReadEditorFile(sheetPath));
            EXPECT_FALSE(sources.back().empty()) << "stylesheet did not read: " << sheetPath;
        }
        if (!appendedCss.empty())
            sources.push_back(appendedCss);
        for (const std::string& css : sources)
        {
            Stylesheet sheet{};
            EXPECT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet));
            Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        }

        auto root = std::make_unique<UIElement>();
        auto plain = std::make_unique<Slider>();
        auto sizeSlider = std::make_unique<GameEngine::EditorUI::ItemSizeSlider>();
        Plain = plain.get();
        SizeSlider = sizeSlider.get();
        root->AddChild(std::move(plain));
        root->AddChild(std::move(sizeSlider));
        Root = root.get();
        Ui->SetRoot(std::move(root));
        Settle();
        return true;
    }

    // Runs frames until a state change has restyled the tree.
    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
    }
};

// The slider's thumb: the child its rules style.
const UIElement* FindThumb(const Slider& slider)
{
    for (const auto& child : slider.GetChildren())
    {
        if (child->HasClass("slider-thumb"))
            return child.get();
    }
    return nullptr;
}

} // namespace

// Redefining the editor's track tokens recolours a plain slider: its track comes from the editor
// theme, not from the engine default that happens to hold the same values.
TEST(EditorSliderTheme, APlainSliderTakesItsTrackFromTheEditorTokens)
{
    SliderThemeFixture fixture;
    if (!fixture.Build(":root { --ui_color_slider_track_fill: #010203; --ui_color_slider_track: #040506;"
                       " --ui_color_slider_tick: #070809; }"))
        GTEST_SKIP() << "no Vulkan device";

    EXPECT_EQ(CustomColor(*fixture.Plain, kTrackFillVar), 0xFF010203u);
    EXPECT_EQ(CustomColor(*fixture.Plain, kTrackVar), 0xFF040506u);
    EXPECT_EQ(CustomColor(*fixture.Plain, kTickVar), 0xFF070809u);
}

// A disabled slider shows its value, not a control: its track loses the accent fill, and its thumb
// takes the neutral colours in every state, set on the slider for the thumb to inherit.
TEST(EditorSliderTheme, ADisabledSliderTakesTheNeutralTrackAndThumb)
{
    SliderThemeFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << "no Vulkan device";
    const uint32_t accentFill = CustomColor(*fixture.Plain, kTrackFillVar);
    fixture.Plain->SetEnabled(false);
    fixture.Settle();

    const uint32_t neutralThumb =
        CustomColor(*fixture.Root, GameEngine::HashStringId("--ui_color_slider_neutral_thumb"));
    const uint32_t neutralGlow =
        CustomColor(*fixture.Root, GameEngine::HashStringId("--ui_color_slider_neutral_thumb_glow"));
    ASSERT_NE(neutralThumb, 0u) << "the neutral thumb token did not resolve; this test is vacuous";
    ASSERT_NE(accentFill, CustomColor(*fixture.Plain, kTrackVar))
        << "the accent fill equals the bare track; this test is vacuous";

    EXPECT_EQ(CustomColor(*fixture.Plain, kTrackFillVar), CustomColor(*fixture.Plain, kTrackVar))
        << "a disabled slider keeps its accent fill";
    for (const char* fill : {"--slider-thumb-fill", "--slider-thumb-fill-hover", "--slider-thumb-fill-focus"})
        EXPECT_EQ(CustomColor(*fixture.Plain, GameEngine::HashStringId(fill)), neutralThumb) << fill;
    for (const char* glow : {"--slider-thumb-glow", "--slider-thumb-glow-hover", "--slider-thumb-glow-focus"})
        EXPECT_EQ(CustomColor(*fixture.Plain, GameEngine::HashStringId(glow)), neutralGlow) << glow;
    const UIElement* thumb = FindThumb(*fixture.Plain);
    ASSERT_NE(thumb, nullptr) << "the slider has no thumb child";
    EXPECT_EQ(thumb->GetResolvedStyle().Visual.BackgroundColor, neutralThumb)
        << "the thumb does not take the colour the disabled slider sets";
}

// The size slider keeps its neutral grey track over the editor's default track colours.
TEST(EditorSliderTheme, TheItemSizeSliderKeepsTheNeutralTrack)
{
    SliderThemeFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << "no Vulkan device";

    const uint32_t neutralFill =
        CustomColor(*fixture.Root, GameEngine::HashStringId("--ui_color_slider_neutral_track_fill"));
    const uint32_t neutralTrack =
        CustomColor(*fixture.Root, GameEngine::HashStringId("--ui_color_slider_neutral_track"));
    ASSERT_NE(neutralFill, 0u) << "the neutral tokens did not resolve; this test is vacuous";
    ASSERT_NE(neutralFill, CustomColor(*fixture.Plain, kTrackFillVar))
        << "the neutral fill equals the default fill; this test is vacuous";

    EXPECT_EQ(CustomColor(*fixture.SizeSlider, kTrackFillVar), neutralFill);
    EXPECT_EQ(CustomColor(*fixture.SizeSlider, kTrackVar), neutralTrack);
}

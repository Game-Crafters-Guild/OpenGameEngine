// The Hierarchy's bottom row: the item size slider beside the panel search. The row is laid out by
// its own stylesheet over the editor theme, and three things about it are easy to lose without any
// structural check noticing: the search has to stay a flexible item beside the slider rather than
// snap back to the panel-anchored placement every other panel search uses; the whole row, not only
// the search, has to move when search bars sit at the top; and the row has to ride above the tree's
// horizontal scrollbar while one is on screen. This file lays the REAL row out over the REAL editor
// stylesheets and measures all three, and checks that the search-bar setting puts its placement
// classes on the row rather than on the search inside it.

#include <gtest/gtest.h>

#include "Editor/Hierarchy/HierarchyNavigationBar.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/EditorSearchBars.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include "TestTempDir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using GameEngine::EditorSearchBars;
using GameEngine::Stylesheet;
using GameEngine::UIElement;
using GameEngine::UIManager;
using GameEngine::Editor::HierarchyNavigationBar;

namespace
{

constexpr uint32_t kViewportW = 400;
constexpr uint32_t kViewportH = 300;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr float kLayoutTolerancePx = 0.01f;

// The row's resting inset from the panel edge and the gap between the two items, as
// HierarchyNavigationBar.css states them, and the width every item size slider shares
// (ItemSizeSlider.css).
constexpr float kRowInsetPx = 8.0f;
constexpr float kSliderWidthPx = 102.0f;
constexpr float kItemGapPx = 8.0f;
// The lift above the tree's horizontal scrollbar: the bar sits 4px above the panel edge and is
// Scrollbar::kDefaultThicknessPx tall, and the row clears it by 3px. Derived from the bar's real
// thickness, so a thickness change the stylesheet's literal misses turns this test red.
constexpr float kScrollbarInsetPx = 4.0f;
constexpr float kRowClearanceAboveScrollbarPx = 3.0f;
constexpr float kRowAboveScrollbarPx =
    kScrollbarInsetPx + GameEngine::Scrollbar::kDefaultThicknessPx + kRowClearanceAboveScrollbarPx;
// Anything narrower than this is a search squeezed out of the row, not one filling it.
constexpr float kMinSearchWidthPx = 100.0f;

// The theme sheets the row sits on, in theme.css import order, then the two control sheets the
// row and its slider request for their own subtrees, which cascade after the theme.
constexpr const char* kSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/core.css",
    "Assets/UI/theme/views.css",
    "Assets/UI/theme/widgets.css",
    "Assets/UI/theme/slider.css",
    "Assets/UI/controls/ItemSizeSlider/ItemSizeSlider.css",
    "Assets/UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.css",
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

struct RowParts
{
    UIElement* Panel = nullptr;
    HierarchyNavigationBar* Row = nullptr;
    UIElement* Slider = nullptr;
    UIElement* Search = nullptr;
};

// A real UIManager over the editor's own stylesheets holding a Hierarchy-shaped panel with the
// real row in it, so a measurement here is the editor's layout and not the test's own CSS.
struct NavigationBarFixture
{
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<UIManager> Ui;
    std::string Diagnostic;
    RowParts Parts;

    bool Build()
    {
        auto panel = std::make_unique<UIElement>();
        panel->AddClass("hierarchy-panel");
        auto row = std::make_unique<HierarchyNavigationBar>();
        auto search = std::make_unique<UIElement>();
        search->AddClass("panel-search-bar");
        Parts.Search = search.get();
        row->SetSearchBar(std::move(search));
        Parts.Row = row.get();
        for (const auto& child : row->GetChildren())
        {
            if (child->HasClass("hierarchy-size-slider"))
                Parts.Slider = child.get();
        }
        panel->AddChild(std::move(row));
        Parts.Panel = panel.get();

        Device = MakeHeadlessDevice();
        if (!Device)
        {
            Diagnostic = "no Vulkan device";
            return false;
        }
        Ui = std::make_unique<UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(kViewportW, kViewportH);
        // An editor sheet that does not read or parse fails the test; only a missing device skips.
        for (const char* sheetPath : kSheets)
        {
            const std::string css = ReadEditorFile(sheetPath);
            EXPECT_FALSE(css.empty()) << "stylesheet did not read: " << sheetPath;
            Stylesheet sheet{};
            EXPECT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet))
                << "stylesheet did not parse: " << sheetPath;
            sheet.SourceName = sheetPath;
            Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        }
        Ui->SetRoot(std::move(panel));
        Settle();
        return true;
    }

    // Update owns style resolution and the Yoga solve; nothing here reads a primitive.
    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
    }
};

float Bottom(const UIElement& element)
{
    return element.GetLayoutY() + element.GetLayoutHeight();
}

float Right(const UIElement& element)
{
    return element.GetLayoutX() + element.GetLayoutWidth();
}

void SetUserDataRoot(const std::string& root)
{
#ifdef _WIN32
    _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.c_str());
#else
    if (root.empty())
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
    else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.c_str(), 1);
#endif
}

// Points the editor preferences at a scratch directory of this process for one test, puts the
// previous root back afterwards so the rest of the process reads the preferences it read before,
// and removes the directory.
struct ScopedUserDataRoot
{
    std::string Previous;
    GameEngine::TestUtils::ScopedTempDir Directory{
        GameEngine::TestUtils::MakeUniqueTempDirectory("HierarchyNavigationBarLayoutTests_user_data")};

    ScopedUserDataRoot()
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            Previous = previous;
        SetUserDataRoot(Directory.Path().string());
    }
    ~ScopedUserDataRoot() { SetUserDataRoot(Previous); }
};

} // namespace

TEST(HierarchyNavigationBarLayout, TheSearchFillsTheRowBesideTheSlider)
{
    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const RowParts& parts = fixture.Parts;

    ASSERT_GT(parts.Panel->GetLayoutHeight(), 0.0f) << "the panel did not lay out; this test is vacuous";
    EXPECT_NEAR(parts.Slider->GetLayoutWidth(), kSliderWidthPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Search->GetLayoutX() - Right(*parts.Slider), kItemGapPx, kLayoutTolerancePx)
        << "the search left the row: it is placed like a panel-anchored search, not beside the slider";
    EXPECT_GT(parts.Search->GetLayoutWidth(), kMinSearchWidthPx) << "the search does not fill the row";
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Row), kRowInsetPx, kLayoutTolerancePx);
}

TEST(HierarchyNavigationBarLayout, TheRowRidesAboveTheHorizontalScrollbar)
{
    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const RowParts& parts = fixture.Parts;

    parts.Row->SetAboveHorizontalScrollbar(true);
    fixture.Settle();
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Row), kRowAboveScrollbarPx, kLayoutTolerancePx);
    EXPECT_NEAR(Bottom(*parts.Search), Bottom(*parts.Row), kLayoutTolerancePx)
        << "the search was lifted inside the row as well as with it";

    parts.Row->SetAboveHorizontalScrollbar(false);
    fixture.Settle();
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Row), kRowInsetPx, kLayoutTolerancePx);
}

// The search-bar setting moves the whole row: its placement classes land on the row and on the
// panel, and the search inside keeps its place in the row.
TEST(HierarchyNavigationBarLayout, SearchBarsAtTheTopMoveTheWholeRow)
{
    // EditorSearchBars persists the setting, so point the preferences at a scratch directory.
    const ScopedUserDataRoot userData;

    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const RowParts& parts = fixture.Parts;

    struct UnregisterOnExit
    {
        UIElement* Search;
        ~UnregisterOnExit()
        {
            EditorSearchBars::SetAtTop(false);
            EditorSearchBars::Unregister(Search);
        }
    } unregister{parts.Search};
    EditorSearchBars::RegisterHosted(parts.Search, parts.Row);
    EditorSearchBars::SetAtTop(true);
    fixture.Settle();

    EXPECT_TRUE(parts.Row->HasClass("search-bar-at-top"));
    EXPECT_TRUE(parts.Panel->HasClass("search-bars-at-top"));
    EXPECT_FALSE(parts.Search->HasClass("search-bar-at-top"))
        << "the search took the placement class, so it is placed inside the row as if it were the row";
    EXPECT_NEAR(parts.Row->GetLayoutY() - parts.Panel->GetLayoutY(), kRowInsetPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Search->GetLayoutY(), parts.Row->GetLayoutY(), kLayoutTolerancePx)
        << "the search moved inside the row instead of with it";
    EXPECT_NEAR(parts.Search->GetLayoutX() - Right(*parts.Slider), kItemGapPx, kLayoutTolerancePx);

    // A horizontal scrollbar at the bottom does not pull a top row down.
    parts.Row->SetAboveHorizontalScrollbar(true);
    fixture.Settle();
    EXPECT_NEAR(parts.Row->GetLayoutY() - parts.Panel->GetLayoutY(), kRowInsetPx, kLayoutTolerancePx);
}

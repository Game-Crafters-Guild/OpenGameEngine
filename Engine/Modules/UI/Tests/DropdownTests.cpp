
#include <gtest/gtest.h>

#include <memory>

#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIPrimitive.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace GameEngine;

namespace
{

static void SendMouse(UIElement& el, EventId id, float x, float y, int button = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

} // namespace

TEST(DropdownTests, UiModeHeaderClickTogglesOpenClass)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two,Three");

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    EXPECT_FALSE(dd.HasClass("open"));
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_TRUE(dd.HasClass("open"));
}

TEST(DropdownTests, UiModeAddsOpenClassToItemsContainer)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two");

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    UIElement* items = dd.GetItemsContainer();
    ASSERT_NE(items, nullptr);

    EXPECT_FALSE(dd.HasClass("open"));
    EXPECT_FALSE(items->HasClass("open"));

    // First click opens the UI list and should mark both the dropdown and the
    // items container with an "open" class so CSS can target either.
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_TRUE(dd.HasClass("open"));
    EXPECT_TRUE(items->HasClass("open"));

    // Second click closes again.
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_FALSE(dd.HasClass("open"));
    EXPECT_FALSE(items->HasClass("open"));
}

TEST(DropdownTests, UiModeItemClickChangesSelection)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two,Three");

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    // Open the UI menu (builds items and adds "open" class).
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_TRUE(dd.HasClass("open"));

    UIElement* items = dd.GetItemsContainer();
    ASSERT_NE(items, nullptr);
    const auto& children = items->GetChildren();
    ASSERT_EQ(children.size(), 3u);
    EXPECT_NE(dynamic_cast<Label*>(children[0].get()), nullptr);
    EXPECT_TRUE(children[0]->HasClass("selected"));

    UIElement* second = children[1].get();
    UILayoutAccess::SetLastLayoutRect(*second, 0.0f, 20.0f, 100.0f, 20.0f);

    SendMouse(*second, kEventMouseDown, 10.0f, 25.0f, 0);

    EXPECT_EQ(dd.GetSelectedIndex(), 1);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("Two"));
    EXPECT_FALSE(children[0]->HasClass("selected"));
    EXPECT_TRUE(children[1]->HasClass("selected"));
    EXPECT_FALSE(dd.HasClass("open"));
}

TEST(DropdownTests, NativeModeUsesInvokerAndUpdatesSelection)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("Red,Green,Blue");
    dd.SetMode(Dropdown::Mode::Native);

    int invokedCount = 0;
    std::vector<std::string> lastLabels;
    int lastSelected = -1;
    std::function<void(int)> onSelected;

    dd.SetNativeMenuInvoker([&invokedCount,
                             &lastLabels,
                             &lastSelected,
                             &onSelected](Dropdown& dropdown, const std::vector<std::string>& labels, int selected, std::function<void(int)> cb)
                            {
        (void)dropdown;
        invokedCount++;
        lastLabels   = labels;
        lastSelected = selected;
        onSelected   = std::move(cb); });

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    // Activating the header in native mode should invoke the platform menu callback.
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);

    EXPECT_EQ(invokedCount, 1);
    ASSERT_EQ(lastLabels.size(), 3u);
    EXPECT_EQ(lastLabels[0], std::string("Red"));
    EXPECT_EQ(lastLabels[2], std::string("Blue"));
    EXPECT_EQ(lastSelected, dd.GetSelectedIndex());

    ASSERT_TRUE(static_cast<bool>(onSelected));
    onSelected(2); // choose "Blue"

    EXPECT_EQ(dd.GetSelectedIndex(), 2);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("Blue"));
}

TEST(DropdownTests, NativeModeWithoutInvokerFallsBackToUiOpenClass)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two");
    dd.SetMode(Dropdown::Mode::Native);

    // Even in native mode, if no invoker is installed the control should
    // gracefully fall back to UI-mode behavior.
    EXPECT_EQ(dd.GetMode(), Dropdown::Mode::Native);

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    EXPECT_FALSE(dd.HasClass("open"));
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_TRUE(dd.HasClass("open"));
}

TEST(DropdownTests, UiModeFocusOutClosesOpenDropdown)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two");

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    // Open the UI dropdown.
    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_TRUE(dd.HasClass("open"));

    UIElement* items = dd.GetItemsContainer();
    ASSERT_NE(items, nullptr);
    EXPECT_TRUE(items->HasClass("open"));

    // Simulate loss of focus as delivered by UIManager via UI.FocusOut.
    UIEvent e{};
    e.Id = kEventFocusOut;
    e.Target = &dd;
    e.CurrentTarget = &dd;
    dd.DispatchEvent(e);

    EXPECT_FALSE(dd.HasClass("open"));
    EXPECT_FALSE(items->HasClass("open"));
}

TEST(DropdownTests, SetSelectedValueUpdatesIndexAndHeader)
{
    Dropdown dd;
    dd.SetOptionsFromString("One,Two,Three");

    EXPECT_EQ(dd.GetSelectedIndex(), 0);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("One"));

    dd.SetSelectedValue("Three");
    EXPECT_EQ(dd.GetSelectedIndex(), 2);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("Three"));

    Label* header = dd.GetHeaderLabel();
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->GetText(), std::string("Three"));
}

TEST(DropdownTests, SetSelectedIndexWithoutNotifyUpdatesStateButFiresNoCallback)
{
    Dropdown dd;
    dd.SetOptionsFromString("One,Two,Three");
    ASSERT_EQ(dd.GetSelectedIndex(), 0);  // SetOptions selects the first option

    // Wire the callback AFTER SetOptions so the initial selection isn't counted.
    int changedCount = 0;
    dd.SetOnValueChanged([&changedCount](const std::string&) { ++changedCount; });

    dd.SetSelectedIndexWithoutNotify(2);
    EXPECT_EQ(dd.GetSelectedIndex(), 2);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("Three"));
    EXPECT_EQ(changedCount, 0);  // suppresses the value-changed callback

    Label* header = dd.GetHeaderLabel();
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->GetText(), std::string("Three"));  // header label follows the selection

    // Equality short-circuit: re-selecting the same index is a no-op.
    dd.SetSelectedIndexWithoutNotify(2);
    EXPECT_EQ(dd.GetSelectedIndex(), 2);
    EXPECT_EQ(changedCount, 0);
}

TEST(DropdownTests, UiModePopupFlipsUpwardNearBottomEdge)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    auto dropdown = std::make_unique<Dropdown>();
    Dropdown* dd = dropdown.get();
    root->AddChild(std::move(dropdown));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0.0f, 0.0f, 400.0f, 400.0f);

    dd->SetOptionsFromString("One,Two,Three");

    // Header near the bottom edge: the estimated popup height does not fit
    // below, and the space above is larger, so the popup opens upward.
    UILayoutAccess::SetLastLayoutRect(*dd, 10.0f, 370.0f, 100.0f, 20.0f);
    dd->OpenMenuUi();
    EXPECT_TRUE(dd->HasClass("open"));
    EXPECT_TRUE(dd->HasClass("open-upward"));
    dd->CloseMenuUi();

    // Plenty of room below: the popup opens downward again.
    UILayoutAccess::SetLastLayoutRect(*dd, 10.0f, 10.0f, 100.0f, 20.0f);
    dd->OpenMenuUi();
    EXPECT_TRUE(dd->HasClass("open"));
    EXPECT_FALSE(dd->HasClass("open-upward"));
}

TEST(DropdownTests, SetModeSwitchesUiAndNative)
{
    Dropdown dd;

    dd.SetMode(Dropdown::Mode::Ui);
    EXPECT_EQ(dd.GetMode(), Dropdown::Mode::Ui);

    dd.SetMode(Dropdown::Mode::Native);
    EXPECT_EQ(dd.GetMode(), Dropdown::Mode::Native);
}

namespace
{
UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}
} // namespace

TEST(DropdownTests, HeaderIconPaintsItsImageWithNoInteraction)
{
    // The closed header is the first thing the user sees. Its slot is created,
    // classed and laid out in one go, so it generates primitives exactly once —
    // and on a first-use image that is before the texture exists. The icon
    // therefore depends entirely on the asset load's completion arriving and
    // invalidating the frame: when that callback is not delivered, the slot
    // stays blank until something unrelated (a hover, a popup) redraws it, and
    // nothing in the control asks again.
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    UIRegistration::RegisterBuiltInControls();

    // An asset root of this test's own, so the icon is genuinely first used
    // here: a texture another test already cached would hide the defect.
    namespace fs = std::filesystem;
    const fs::path assetRoot = fs::temp_directory_path() / "dropdown_header_icon_paint";
    std::error_code ec;
    fs::remove_all(assetRoot, ec);
    fs::create_directories(assetRoot, ec);
    const fs::path iconPath = assetRoot / "probe-icon.svg";
    {
        std::ofstream out(iconPath, std::ios::binary);
        out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" )"
            << R"(viewBox="0 0 16 16"><rect x="2" y="2" width="12" height="12" fill="#FFFFFF"/></svg>)";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(iconPath));

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    UIManager ui(device, &assets);
    ui.SetRoot(std::move(root));

    static constexpr const char* kProbeCss = R"CSS(
#root { width: 400px; height: 200px; }
.dropdown-header-icon { width: 16px; height: 16px; }
.dropdown-icon--probe { background-image: url("probe-icon.svg"); }
)CSS";
    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(kProbeCss, sheet));
    ui.AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));

    auto owned = std::make_unique<Dropdown>();
    Dropdown* dropdown = owned.get();
    ui.GetRootElement()->AddChild(std::move(owned));
    dropdown->SetOptions({{"a", "First", {}, "dropdown-icon--probe"}});

    UIElement* icon = ChildWithClass(*dropdown->GetHeaderContainer(), "dropdown-header-icon");
    ASSERT_NE(icon, nullptr);

    // Frames and nothing else: no hover, no popup, no restyle.
    UiRgHarness rg(device);
    const auto drawsItsImage = [&]()
    {
        for (uint16_t i = 0;; ++i)
        {
            const UI::UIPrimitive* primitive = ui.PeekPrimitiveForTesting(*icon, i);
            if (!primitive)
                return false;
            if (UI::GetMode(primitive->ModeAndFlags) == UI::PrimitiveMode::Textured)
                return true;
        }
    };
    // Two waits, separated on purpose. The defect under test is that the arrival
    // never reaches the frame, NOT that the loader is slow, so the asset gets a
    // wall-clock deadline of its own — a frame budget alone races the thread pool
    // on a loaded machine.
    const GUID iconGuid = assets.GetRegistry().GetAssetGUID(iconPath);
    ASSERT_FALSE(iconGuid.IsNull());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int frames = 0;
    bool painted = false;
    while (!assets.GetAsset(iconGuid) && std::chrono::steady_clock::now() < deadline)
    {
        ui.Update(0.001f, /*interactive=*/true);
        DriveUiRender(ui, rg);
        ++frames;
        painted = painted || drawsItsImage();
    }
    ASSERT_TRUE(assets.GetAsset(iconGuid)) << "the icon asset never loaded in 20 s";
    for (int i = 0; i < 240 && !painted; ++i)
    {
        ui.Update(0.001f, /*interactive=*/true);
        DriveUiRender(ui, rg);
        ++frames;
        painted = drawsItsImage();
    }
    EXPECT_TRUE(painted) << "the header icon never drew its image (" << frames << " frames)";
    EXPECT_EQ(ui.PendingBackgroundImageCount(), 0u) << "a background image is still in flight";

    fs::remove_all(assetRoot, ec);
}

TEST(DropdownTests, ANarrowPanelPullsThePopupBackInside)
{
    // A fit-content popup is anchored to the header's left edge, so in a panel
    // narrower than the popup its right edge leaves the panel and the rows are
    // clipped at the boundary instead of being readable.
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(
        1.0f,
        R"(<uielement id="root"><uielement id="pane" class="panel"><uielement id="label"/><uielement id="host"/></uielement></uielement>)",
        R"(
#root { width: 400px; height: 300px; flex-direction: row; }
/* An inspector row: the field sits in the right-hand column, so its popup
   starts most of the way across an already narrow panel. */
#pane { width: 150px; height: 300px; flex-direction: row; }
#label { width: 90px; }
#host { width: 60px; }
.dropdown { min-width: 60px; }
.dropdown-items { position: absolute; top: 100%; left: 0px; width: 100%; }
.dropdown-items.fit-content { width: auto; min-width: 100%; }
.dropdown-item { padding: 2px 10px 2px 24px; }
)");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();

    UIElement* host = fixture.Element("host");
    UIElement* pane = fixture.Element("pane");
    ASSERT_NE(host, nullptr);
    ASSERT_NE(pane, nullptr);

    auto owned = std::make_unique<Dropdown>();
    Dropdown* dropdown = owned.get();
    host->AddChild(std::move(owned));
    dropdown->SetOptions({{"a", "Physical (camera)", {}, "probe-icon"},
                          {"b", "Manual (EV100)", {}, "probe-icon"},
                          {"c", "Auto (metered)", {}, "probe-icon"}});
    fixture.Settle();
    dropdown->OpenMenuUi();
    fixture.Settle();

    UIElement* items = dropdown->GetItemsContainer();
    ASSERT_NE(items, nullptr);
    const float paneLeft = pane->GetLayoutX();
    const float paneRight = paneLeft + pane->GetLayoutWidth();
    const float popupLeft = items->GetLayoutX();
    const float popupRight = popupLeft + items->GetLayoutWidth();
    EXPECT_LE(popupRight, paneRight + 0.5f)
        << "popup right " << popupRight << " leaves the panel at " << paneRight;
    EXPECT_GE(popupLeft, paneLeft - 0.5f)
        << "popup left " << popupLeft << " starts before the panel at " << paneLeft;
}

TEST(DropdownTests, HeaderIconResolvesItsImageWithNoInteraction)
{
    // The closed header is the first thing the user sees, so the slot must
    // resolve its image from the option's class with no popup ever opened.
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(
        1.0f, R"(<uielement id="root"><uielement id="host"/></uielement>)",
        R"(
#root { width: 400px; height: 200px; }
.dropdown-icon--probe { background-image: url("editor:Icons/Probe/first.svg"); }
)");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();

    UIElement* host = fixture.Element("host");
    ASSERT_NE(host, nullptr);
    auto owned = std::make_unique<Dropdown>();
    Dropdown* dropdown = owned.get();
    host->AddChild(std::move(owned));
    dropdown->SetOptions({{"a", "First", {}, "dropdown-icon--probe"}});
    fixture.Settle();

    UIElement* icon = ChildWithClass(*dropdown->GetHeaderContainer(), "dropdown-header-icon");
    ASSERT_NE(icon, nullptr);
    const BackgroundImageSource& source = icon->GetResolvedStyle().Visual.BackgroundImage.Source;
    EXPECT_EQ(source.Kind, BackgroundImageSource::SourceKind::Path);
    EXPECT_EQ(source.Value, "Icons/Probe/first.svg");
    EXPECT_EQ(source.SourceAlias, "editor");
}

TEST(DropdownTests, OptionIconClassReachesHeaderAndRows)
{
    Dropdown dropdown;
    dropdown.SetOptions({{"a", "First", {}, "test-icon-first"},
                         {"b", "Second", {}, "test-icon-second"}});

    UIElement* headerIcon = ChildWithClass(*dropdown.GetHeaderContainer(), "dropdown-header-icon");
    ASSERT_NE(headerIcon, nullptr);
    EXPECT_TRUE(headerIcon->HasClass("test-icon-first"));
    // The control names a class and nothing else: no image, tint or visibility
    // is decided here, so the stylesheet stays the only authority on appearance.
    EXPECT_FALSE(headerIcon->Overrides().Get(Style::BackgroundImage).has_value());

    int changes = 0;
    dropdown.SetOnValueChanged([&](const std::string& value) { ++changes; EXPECT_EQ(value, "b"); });
    dropdown.SetSelectedIndex(1);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(dropdown.GetSelectedValue(), "b");
    EXPECT_EQ(dropdown.GetHeaderLabel()->GetText(), "Second");
    EXPECT_FALSE(headerIcon->HasClass("test-icon-first"));
    EXPECT_TRUE(headerIcon->HasClass("test-icon-second"));

    dropdown.OpenMenuUi();
    EXPECT_EQ(changes, 1) << "Opening the popup must not commit a value";
    const auto& rows = dropdown.GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    const char* expected[] = {"test-icon-first", "test-icon-second"};
    const char* text[] = {"First", "Second"};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        UIElement* icon = ChildWithClass(*rows[i], "dropdown-item-icon");
        ASSERT_NE(icon, nullptr) << "row " << i;
        EXPECT_TRUE(icon->HasClass(expected[i]));
        auto* label = dynamic_cast<Label*>(ChildWithClass(*rows[i], "dropdown-item-label"));
        ASSERT_NE(label, nullptr) << "row " << i;
        EXPECT_EQ(label->GetText(), text[i]);
        EXPECT_TRUE(label->GetChildren().empty()) << "option text must stay a measure leaf";
    }
}

TEST(DropdownTests, UnannotatedOptionsCarryNoIconSlot)
{
    Dropdown dropdown;
    dropdown.SetOptionsFromString("One,Two");

    EXPECT_EQ(ChildWithClass(*dropdown.GetHeaderContainer(), "dropdown-header-icon"), nullptr);
    dropdown.OpenMenuUi();
    for (const auto& row : dropdown.GetItemsContainer()->GetChildren())
    {
        EXPECT_EQ(ChildWithClass(*row, "dropdown-item-icon"), nullptr);
        // Without an icon the row keeps its own text, so it stays the Yoga
        // text-measure leaf it has always been.
        EXPECT_FALSE(dynamic_cast<Label*>(row.get())->GetText().empty());
        EXPECT_TRUE(row->GetChildren().empty());
    }
}

TEST(DropdownTests, AnIconBearingPopupFitsItsContent)
{
    // The header sizes the popup, so a column added by the control would come
    // out of the labels and wrap them. Only the icon case is the control's to
    // decide; a caller's own auto-width choice is untouched.
    Dropdown dropdown;
    dropdown.SetOptionsFromString("One,Two");
    EXPECT_FALSE(dropdown.GetItemsContainer()->HasClass("fit-content"));
    EXPECT_FALSE(dropdown.GetAutoWidthPopup());

    dropdown.SetOptions({{"a", "First", {}, "test-icon-first"}});
    EXPECT_TRUE(dropdown.GetItemsContainer()->HasClass("fit-content"));
    EXPECT_FALSE(dropdown.GetAutoWidthPopup());

    dropdown.SetOptions({{"a", "First"}});
    EXPECT_FALSE(dropdown.GetItemsContainer()->HasClass("fit-content"));
}

TEST(DropdownTests, ClearingTheIconsRemovesTheHeaderSlot)
{
    Dropdown dropdown;
    dropdown.SetOptions({{"a", "First", {}, "test-icon-first"}});
    ASSERT_NE(ChildWithClass(*dropdown.GetHeaderContainer(), "dropdown-header-icon"), nullptr);

    // A control whose new options declare nothing gives the column back rather
    // than keeping an empty slot in the header's spacing.
    dropdown.SetOptions({{"a", "First"}});
    EXPECT_EQ(ChildWithClass(*dropdown.GetHeaderContainer(), "dropdown-header-icon"), nullptr);
}

TEST(DropdownTests, PartiallyAnnotatedOptionsKeepTheIconColumn)
{
    Dropdown dropdown;
    dropdown.SetOptions({{"a", "Annotated", {}, "test-icon-first"}, {"b", "Plain", {}}});
    dropdown.OpenMenuUi();

    const auto& rows = dropdown.GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    UIElement* plainIcon = ChildWithClass(*rows[1], "dropdown-item-icon");
    ASSERT_NE(plainIcon, nullptr) << "an unannotated row still reserves the column so text aligns";
    EXPECT_EQ(plainIcon->GetClasses().size(), 1u);

    // Selecting the unannotated option clears the header icon's art class.
    UIElement* headerIcon = ChildWithClass(*dropdown.GetHeaderContainer(), "dropdown-header-icon");
    ASSERT_NE(headerIcon, nullptr);
    EXPECT_TRUE(headerIcon->HasClass("test-icon-first"));
    dropdown.SetSelectedIndex(1);
    EXPECT_FALSE(headerIcon->HasClass("test-icon-first"));
}

TEST(DropdownTests, OptionColorLandsOnTheTextOfAnIconRow)
{
    Dropdown dropdown;
    dropdown.SetOptions({{"a", "Selected", "#ff0000", "test-icon-first"},
                         {"b", "Resting", "#00ff00", "test-icon-second"}});
    dropdown.OpenMenuUi();

    const auto& rows = dropdown.GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    // The selected row takes its color from the stylesheet, a resting row from
    // the option — and with an icon the text is a child, not the row itself.
    auto* selectedLabel = dynamic_cast<Label*>(ChildWithClass(*rows[0], "dropdown-item-label"));
    ASSERT_NE(selectedLabel, nullptr);
    EXPECT_FALSE(selectedLabel->Overrides().Get(Style::Color).has_value());
    auto* restingLabel = dynamic_cast<Label*>(ChildWithClass(*rows[1], "dropdown-item-label"));
    ASSERT_NE(restingLabel, nullptr);
    EXPECT_EQ(restingLabel->Overrides().Get(Style::Color), 0xff00ff00u);
}

TEST(DropdownTests, EnumEntryIconClassReachesTheDropdown)
{
    enum class Mode { One, Two };
    const EnumEntry<Mode> entries[] = {
        {Mode::One, "One", "test-icon-first"},
        {Mode::Two, "Two", "test-icon-second"},
    };
    EnumField<Mode> field;
    field.SetEntries(entries, Mode::One);

    Dropdown* dropdown = field.GetDropdown();
    ASSERT_NE(dropdown, nullptr);
    UIElement* headerIcon = ChildWithClass(*dropdown->GetHeaderContainer(), "dropdown-header-icon");
    ASSERT_NE(headerIcon, nullptr);
    EXPECT_TRUE(headerIcon->HasClass("test-icon-first"));

    Mode selected = Mode::One;
    field.SetOnValueChanged([&](Mode mode) { selected = mode; });
    dropdown->SetSelectedIndex(1);
    EXPECT_EQ(selected, Mode::Two);
    EXPECT_EQ(dropdown->GetHeaderLabel()->GetText(), "Two");
    EXPECT_TRUE(headerIcon->HasClass("test-icon-second"));
}

// An inspector row that re-labels its options when the asset it shows changes
// state rebuilds the entry list and re-selects the value it already holds. That
// must be silent: a notification there is indistinguishable from a user edit, so
// it commits a write, which reloads the asset, which rebuilds the row — the loop
// that killed an editor session before the panel's value sources were reduced to
// one each. The contract is two-sided: the same value never notifies, a changed
// value notifies exactly once.
TEST(DropdownTests, ReSelectingTheSameValueNotifiesNobody)
{
    Dropdown dd;
    dd.SetOptionsFromString("One,Two,Three");
    dd.SetSelectedIndex(1);
    ASSERT_EQ(dd.GetSelectedValue(), std::string("Two"));

    // Wired after the initial selection, as an inspector wires its rows: the
    // build-time selection is not an edit either.
    int changes = 0;
    dd.SetOnValueChanged([&changes](const std::string&) { ++changes; });

    dd.SetSelectedIndex(1);
    EXPECT_EQ(changes, 0);

    // The list rebuilt with the same values and the same selection, which is
    // what a re-labelled row does. (The label-only overloads reselect the first
    // option, so a row that rebuilds has to name the selection it wants.)
    const std::vector<Dropdown::Option> sameOptions = {{"One", "One"}, {"Two", "Two"}, {"Three", "Three"}};
    dd.SetOptions(sameOptions, 1);
    EXPECT_EQ(changes, 0) << "rebuilding the list on the same value is not an edit";

    dd.SetSelectedIndex(2);
    EXPECT_EQ(changes, 1) << "a value the user did change notifies once";
    EXPECT_EQ(dd.GetSelectedValue(), std::string("Three"));
}

TEST(DropdownTests, SetEntriesWithTheValueAlreadySelectedNotifiesNobody)
{
    enum class Usage { Auto, Color, Mask, Packed };
    const EnumEntry<Usage> plain[] = {
        {Usage::Auto, "Auto"},
        {Usage::Color, "Color"},
        {Usage::Mask, "Mask"},
        {Usage::Packed, "Packed data"},
    };
    // The same values, relabelled — the shape an inspector row takes when the
    // asset it describes turns out to carry no authored value of its own.
    const EnumEntry<Usage> relabelled[] = {
        {Usage::Auto, "Not set"},
        {Usage::Color, "Color (sRGB)"},
        {Usage::Mask, "Mask (single channel)"},
        {Usage::Packed, "Packed data (ORM)"},
    };

    EnumField<Usage> field;
    field.SetEntries(plain, Usage::Mask);

    int changes = 0;
    Usage lastSeen = Usage::Auto;
    field.SetOnValueChanged([&](Usage value) { ++changes; lastSeen = value; });

    field.SetEntries(relabelled, Usage::Mask);
    EXPECT_EQ(changes, 0) << "the row was re-labelled, not edited";

    field.SetEntries(relabelled, Usage::Packed);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(lastSeen, Usage::Packed);
}

// A dropdown inside a popover that sets a z-index above its list's: the open list paints
// over the popover (a nested overlay paints after the overlay that holds it), so a press on
// an item must reach the item, not the popover under it.
TEST(DropdownTests, AnItemOfAListOpenInsideAPopoverTakesThePress)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(
        1.0f,
        R"(<uielement id="root"><uielement id="popover"><uielement id="host"/></uielement></uielement>)",
        R"(
#root { width: 400px; height: 300px; }
#popover { position: absolute; left: 20px; top: 20px; width: 300px; height: 250px; z-index: 10000; }
#host { width: 120px; }
.dropdown-items { position: absolute; top: 100%; left: 0px; width: 100%; z-index: 1000; }
.dropdown-item { height: 20px; }
)");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();

    UIElement* popover = fixture.Element("popover");
    ASSERT_NE(popover, nullptr);
    popover->SetOverlayLayer(OverlayLayer::Dropdown);
    auto owned = std::make_unique<Dropdown>();
    Dropdown* dropdown = owned.get();
    fixture.Element("host")->AddChild(std::move(owned));
    dropdown->SetOptionsFromString("One,Two,Three");
    fixture.Settle();
    dropdown->OpenMenuUi();
    fixture.Settle();

    UIElement* items = dropdown->GetItemsContainer();
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->GetChildren().size(), 3u);
    const UIElement* second = items->GetChildren()[1].get();
    const float x = second->GetLayoutX() + second->GetLayoutWidth() * 0.5f;
    const float y = second->GetLayoutY() + second->GetLayoutHeight() * 0.5f;
    ASSERT_GT(second->GetLayoutHeight(), 0.0f);
    ASSERT_TRUE(x > popover->GetLayoutX() && y < popover->GetLayoutY() + popover->GetLayoutHeight())
        << "the item lies over the popover";

    fixture.Manager().OnMouseMove(x, y);
    fixture.StepFrame();
    fixture.Manager().OnMouseButton(0, true);
    fixture.StepFrame();
    fixture.Manager().OnMouseButton(0, false);
    fixture.StepFrame();
    EXPECT_EQ(dropdown->GetSelectedValue(), std::string("Two"));
}

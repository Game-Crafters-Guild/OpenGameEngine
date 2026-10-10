// A disabled control never activates, and focus never reaches it.
//
// Two halves, both pinned here. Every control that carries a value refuses the
// activation itself, so an event delivered to it directly changes nothing. And
// the UI manager treats a disabled element the way every browser treats a
// disabled form control: a press, release, key or focus request never reaches
// it, Tab passes over it, and a control disabled while focused is blurred, while
// hover, and so its tooltip, and the wheel still do. A disabled container carries the same
// answer for everything inside it, as a disabled fieldset does.

#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <filesystem>
#include <fstream>
#include <memory>

#ifndef GLFW_KEY_TAB
#define GLFW_KEY_TAB 258
#endif
#ifndef GLFW_PRESS
#define GLFW_PRESS 1
#endif

using namespace GameEngine;

namespace
{

void SendMouse(UIElement& el, EventId id, float x, float y, int button = 0)
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

void SendKey(UIElement& el, int key)
{
    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = key;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

TextInput* FindEditor(FloatField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

// The focus tests below run a real UIManager: the walks that build the tab
// order read resolved styles, so the elements need a stylesheet and one update
// before the order means anything.
std::filesystem::path WriteFocusRowStylesheet()
{
    const std::filesystem::path css =
        std::filesystem::temp_directory_path() / "ui_disabled_control_focus_row.css";
    std::ofstream file(css);
    file << "#root { display: flex; width: 400px; height: 40px; }\n"
         << "#group { display: flex; width: 200px; height: 20px; }\n"
         << ".focus-row-field { width: 60px; height: 20px; }\n";
    return css;
}

std::unique_ptr<TextField> MakeFocusRowField(const char* id)
{
    auto field = std::make_unique<TextField>();
    field->SetId(id);
    field->AddClass("focus-row-field");
    return field;
}

} // namespace

TEST(DisabledControlInputTests, DisabledDropdownDoesNotOpenOnEnter)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two,Three");
    dd.SetDisabled(true);

    SendKey(dd, Input::kKeyCode_Enter);
    EXPECT_FALSE(dd.IsMenuOpen());
    EXPECT_FALSE(dd.HasClass("open"));

    SendKey(dd, Input::kKeyCode_Space);
    EXPECT_FALSE(dd.IsMenuOpen());
    EXPECT_FALSE(dd.HasClass("open"));
}

TEST(DisabledControlInputTests, DisabledDropdownDoesNotOpenOnHeaderClick)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two,Three");
    dd.SetDisabled(true);

    UIElement* header = dd.GetHeaderContainer();
    ASSERT_NE(header, nullptr);
    UILayoutAccess::SetLastLayoutRect(*header, 0.0f, 0.0f, 100.0f, 20.0f);

    SendMouse(*header, kEventMouseDown, 5.0f, 10.0f, 0);
    EXPECT_FALSE(dd.IsMenuOpen());
    EXPECT_FALSE(dd.HasClass("open"));
}

TEST(DisabledControlInputTests, ReEnabledDropdownOpensAgain)
{
    Dropdown dd;
    UILayoutAccess::SetLastLayoutRect(dd, 0.0f, 0.0f, 100.0f, 20.0f);
    dd.SetOptionsFromString("One,Two,Three");

    dd.SetDisabled(true);
    SendKey(dd, Input::kKeyCode_Enter);
    ASSERT_FALSE(dd.HasClass("open"));

    dd.SetDisabled(false);
    SendKey(dd, Input::kKeyCode_Enter);
    EXPECT_TRUE(dd.HasClass("open"));
}

TEST(DisabledControlInputTests, DisabledToggleDoesNotFlipOnClickOrSpace)
{
    Toggle toggle;
    UILayoutAccess::SetLastLayoutRect(toggle, 0.0f, 0.0f, 30.0f, 17.0f);
    toggle.SetChecked(true);
    toggle.SetDisabled(true);

    SendMouse(toggle, kEventMouseDown, 10.0f, 8.0f, 0);
    SendMouse(toggle, kEventMouseUp, 10.0f, 8.0f, 0);
    EXPECT_TRUE(toggle.IsChecked());

    SendKey(toggle, Input::kKeyCode_Space);
    EXPECT_TRUE(toggle.IsChecked());

    SendKey(toggle, Input::kKeyCode_Enter);
    EXPECT_TRUE(toggle.IsChecked());
}

TEST(DisabledControlInputTests, DisabledTogglePressLeavesNoPressedClass)
{
    Toggle toggle;
    UILayoutAccess::SetLastLayoutRect(toggle, 0.0f, 0.0f, 30.0f, 17.0f);
    toggle.SetDisabled(true);

    SendMouse(toggle, kEventMouseDown, 10.0f, 8.0f, 0);
    EXPECT_FALSE(toggle.HasClass("pressed"));
}

TEST(DisabledControlInputTests, ReEnabledToggleFlipsAgain)
{
    Toggle toggle;
    UILayoutAccess::SetLastLayoutRect(toggle, 0.0f, 0.0f, 30.0f, 17.0f);

    toggle.SetDisabled(true);
    SendKey(toggle, Input::kKeyCode_Space);
    ASSERT_FALSE(toggle.IsChecked());

    toggle.SetDisabled(false);
    SendKey(toggle, Input::kKeyCode_Space);
    EXPECT_TRUE(toggle.IsChecked());
}

TEST(DisabledControlInputTests, DisabledFloatFieldRefusesTypedInput)
{
    FloatField field;
    field.SetValueWithoutNotify(3.0f);
    field.SetDisabled(true);

    EXPECT_FALSE(field.OnChar('7'));
    EXPECT_FLOAT_EQ(field.GetValue(), 3.0f);

    EXPECT_FALSE(field.OnKey(Input::kKeyCode_Backspace, 0, nullptr));
    EXPECT_FLOAT_EQ(field.GetValue(), 3.0f);
}

TEST(DisabledControlInputTests, DisabledFloatFieldDoesNotScrubOnDrag)
{
    FloatField field;
    field.SetValueWithoutNotify(3.0f);
    field.EnableDragToChange();
    field.SetDisabled(true);

    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);

    UIEvent down{};
    down.Id = kEventMouseDown;
    down.X = 0.0f;
    down.Target = editor;
    down.CurrentTarget = editor;
    editor->DispatchEvent(down);

    UIEvent move{};
    move.Id = kEventMouseMove;
    move.X = 200.0f;
    move.Target = editor;
    move.CurrentTarget = editor;
    editor->DispatchEvent(move);

    EXPECT_FLOAT_EQ(field.GetValue(), 3.0f);
}

TEST(DisabledControlInputTests, ReEnabledFloatFieldAcceptsTypedInput)
{
    FloatField field;
    field.SetValueWithoutNotify(3.0f);

    field.SetDisabled(true);
    ASSERT_FALSE(field.OnChar('7'));

    field.SetDisabled(false);
    EXPECT_TRUE(field.OnChar('7'));
}

TEST(DisabledControlInputTests, TabPassesOverADisabledControl)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddChild(MakeFocusRowField("first"));
    auto refused = MakeFocusRowField("refused");
    refused->SetDisabled(true);
    root->AddChild(std::move(refused));
    root->AddChild(MakeFocusRowField("last"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(WriteFocusRowStylesheet().string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("first"));

    // Tab reaches the next ENABLED control instead of stopping on the disabled
    // one it passes: the rule every browser applies to a disabled form control.
    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("last"));
}

TEST(DisabledControlInputTests, TabPassesOverEveryControlInsideADisabledContainer)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddChild(MakeFocusRowField("first"));

    // The gated-group shape an inspector builds: a container turned off as one
    // unit, holding controls that are each enabled in their own right.
    auto group = std::make_unique<UIElement>();
    group->SetId("group");
    group->AddChild(MakeFocusRowField("inGroupOne"));
    group->AddChild(MakeFocusRowField("inGroupTwo"));
    group->SetDisabled(true);
    root->AddChild(std::move(group));

    root->AddChild(MakeFocusRowField("last"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(WriteFocusRowStylesheet().string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("first"));

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("last"));
}

TEST(DisabledControlInputTests, DisablingTheFocusedControlTakesFocusOffIt)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto first = MakeFocusRowField("first");
    TextField* firstRaw = first.get();
    root->AddChild(std::move(first));
    root->AddChild(MakeFocusRowField("last"));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(WriteFocusRowStylesheet().string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    ASSERT_EQ(ui.GetFocusedElementId(), std::string("first"));

    firstRaw->SetDisabled(true);
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    EXPECT_TRUE(ui.GetFocusedElementId().empty());
}

TEST(DisabledControlInputTests, DisablingAContainerTakesFocusOffTheControlInsideIt)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto group = std::make_unique<UIElement>();
    group->SetId("group");
    group->AddChild(MakeFocusRowField("inGroupOne"));
    UIElement* groupRaw = group.get();
    root->AddChild(std::move(group));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(WriteFocusRowStylesheet().string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    ASSERT_EQ(ui.GetFocusedElementId(), std::string("inGroupOne"));

    groupRaw->SetDisabled(true);
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    EXPECT_TRUE(ui.GetFocusedElementId().empty());
}

TEST(DisabledControlInputTests, PressingADisabledControlDoesNotFocusIt)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddChild(MakeFocusRowField("first"));
    auto refused = MakeFocusRowField("refused");
    TextField* refusedRaw = refused.get();
    refused->SetDisabled(true);
    root->AddChild(std::move(refused));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromFile(WriteFocusRowStylesheet().string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    ui.OnKey(GLFW_KEY_TAB, GLFW_PRESS, 0);
    DriveUiRender(ui, rg);
    ASSERT_EQ(ui.GetFocusedElementId(), std::string("first"));

    ui.OnMouseMove(refusedRaw->GetLayoutX() + 5.0f, refusedRaw->GetLayoutY() + 5.0f);
    ui.OnMouseButton(0, true);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);
    ui.OnMouseButton(0, false);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);

    EXPECT_NE(ui.GetFocusedElementId(), std::string("refused"));
}

namespace
{

// A real UIManager with an enabled toggle inside the #group container, laid out by the focus-row
// stylesheet, so presses go through the pointer router and its hit test.
struct ContainedToggle
{
    std::unique_ptr<UiRgHarness> Rg;
    std::unique_ptr<UIManager> Ui;
    UIElement* Group = nullptr;
    Toggle* Control = nullptr;

    bool Build(Rendering::IDevice* dev)
    {
        Rg = std::make_unique<UiRgHarness>(dev);
        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto group = std::make_unique<UIElement>();
        group->SetId("group");
        auto toggle = std::make_unique<Toggle>();
        toggle->SetId("contained-toggle");
        toggle->AddClass("focus-row-field");
        Control = toggle.get();
        group->AddChild(std::move(toggle));
        Group = group.get();
        root->AddChild(std::move(group));
        Ui = std::make_unique<UIManager>(dev);
        Ui->SetRoot(std::move(root));
        if (!Ui->AttachStyleFromFile(WriteFocusRowStylesheet().string()))
            return false;
        Frame();
        return true;
    }
    void Frame()
    {
        Ui->Update(0.0f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }
    void MoveOver(const UIElement& el)
    {
        Ui->OnMouseMove(el.GetLayoutX() + 5.0f, el.GetLayoutY() + 5.0f);
        Frame();
    }
    void Press(bool down)
    {
        Ui->OnMouseButton(0, down);
        Frame();
    }
    void Click(const UIElement& el)
    {
        MoveOver(el);
        Press(true);
        Press(false);
    }
};

} // namespace

TEST(DisabledControlInputTests, AClickOnAnEnabledToggleInsideADisabledContainerChangesNothing)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    t.Group->SetEnabled(false);
    t.Frame();

    t.Click(*t.Control);
    EXPECT_FALSE(t.Control->IsChecked()) << "a click reached a control inside a disabled container";
    EXPECT_FALSE(t.Ui->IsMouseCaptured());
}

TEST(DisabledControlInputTests, ReEnablingTheContainerLetsTheSameClickFlipTheToggle)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    t.Group->SetEnabled(false);
    t.Frame();
    t.Click(*t.Control);
    ASSERT_FALSE(t.Control->IsChecked());

    t.Group->SetEnabled(true);
    t.Frame();
    t.Click(*t.Control);
    EXPECT_TRUE(t.Control->IsChecked());
}

// A press that started on an enabled control keeps its capture when the container is disabled
// mid-press, and the release then ends the gesture as a cancel: it lets go and completes nothing.
TEST(DisabledControlInputTests, APressAcrossTheContainerBeingDisabledEndsAsACancel)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    t.MoveOver(*t.Control);
    t.Press(true);
    ASSERT_TRUE(t.Ui->IsMouseCaptured()) << "the toggle did not take the press";

    t.Group->SetEnabled(false);
    t.Frame();
    t.MoveOver(*t.Control);
    t.Press(false);
    EXPECT_FALSE(t.Ui->IsMouseCaptured()) << "the capture outlived the release";
    EXPECT_FALSE(t.Control->IsChecked()) << "the release completed a click the disable should have cancelled";
    EXPECT_FALSE(t.Control->HasClass("pressed"));
}

// Hover still reaches a disabled control inside a disabled row, so the reason it carries as its
// tooltip (InspectorUI::DisableRowOfControl) still shows.
TEST(DisabledControlInputTests, HoverStillFindsTheTooltipOfADisabledControlInADisabledContainer)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    t.Control->SetTooltip("Driven by the sky");
    t.Control->SetEnabled(false);
    t.Group->SetEnabled(false);
    t.Frame();

    t.MoveOver(*t.Control);
    // The tooltip overlay takes the first authored tooltip on the way up from the hovered element
    // (TooltipOverlay::FindTooltipText); hover lands on the control's knob, inside the control.
    const UIElement* source = t.Ui->GetHoveredElement();
    while (source && source->GetTooltip().empty())
        source = source->GetParent();
    ASSERT_EQ(source, t.Control) << "hover did not reach the disabled control";
    EXPECT_EQ(source->GetTooltip(), "Driven by the sky");
}

// Programmatic focus follows the same rule as Tab and a press: a control inside a disabled
// container, or mounted into a disabled host (a mount's content counts as the host's own),
// does not take it, and focus stays where it was.
TEST(DisabledControlInputTests, FocusingAControlInsideADisabledContainerOrMountLeavesFocusWhereItWas)
{
    UIManager ui{nullptr};
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto kept = std::make_unique<UIElement>();
    kept->SetId("kept");
    root->AddChild(std::move(kept));
    auto group = std::make_unique<UIElement>();
    group->SetId("group");
    auto inner = std::make_unique<UIElement>();
    inner->SetId("inner");
    group->AddChild(std::move(inner));
    UIElement* groupRaw = group.get();
    root->AddChild(std::move(group));
    auto mountOwned = std::make_unique<Mount>();
    Mount* mount = mountOwned.get();
    root->AddChild(std::move(mountOwned));
    ui.SetRoot(std::move(root));
    // Externally owned, like a docked panel: the Mount does not own its target.
    auto panel = std::make_unique<UIElement>();
    auto mounted = std::make_unique<UIElement>();
    mounted->SetId("mounted");
    panel->AddChild(std::move(mounted));
    mount->SetTarget(panel.get());

    ui.SetFocusById("kept");
    groupRaw->SetEnabled(false);
    ui.SetFocusById("inner");
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("kept")) << "focus moved into a disabled container";

    mount->SetEnabled(false);
    ui.SetFocusById("mounted");
    EXPECT_EQ(ui.GetFocusedElementId(), std::string("kept")) << "focus moved into a disabled mount host";
    mount->SetTarget(nullptr);
}

// The DisableRowOfControl shape: the control itself is disabled along with its container while a
// press on it is held. The cancel at the release still disarms it, so after both are enabled again
// a release that comes from a press elsewhere does not flip it.
TEST(DisabledControlInputTests, AHeldPressOnAToggleDisabledWithItsContainerLeavesNoStaleArm)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    t.MoveOver(*t.Control);
    t.Press(true);
    ASSERT_TRUE(t.Ui->IsMouseCaptured()) << "the toggle did not take the press";
    t.Control->SetEnabled(false);
    t.Group->SetEnabled(false);
    t.Frame();
    t.Press(false);
    EXPECT_FALSE(t.Control->HasClass("pressed")) << "the cancel did not disarm the disabled toggle";

    t.Control->SetEnabled(true);
    t.Group->SetEnabled(true);
    t.Frame();
    t.Ui->OnMouseMove(300.0f, 30.0f); // the root, outside the group
    t.Frame();
    t.Press(true);
    t.MoveOver(*t.Control);
    t.Press(false);
    EXPECT_FALSE(t.Control->IsChecked()) << "a release with no press of its own flipped the toggle";
    EXPECT_FALSE(t.Control->HasClass("pressed"));
}

// Input aimed into a disabled group goes to the first ancestor outside it, so a panel around the
// group still hears a press (a floating panel raises itself on one) and the keys of the hover
// fallback.
TEST(DisabledControlInputTests, AnAncestorOutsideADisabledGroupStillHearsAPressAndAKey)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    ContainedToggle t;
    ASSERT_TRUE(t.Build(dev));
    int rootDowns = 0;
    int rootKeys = 0;
    t.Ui->GetRootElement()->RegisterEventHandler(kEventMouseDown, [&rootDowns](UIEvent&) { ++rootDowns; });
    t.Ui->GetRootElement()->RegisterEventHandler(kEventKeyDown, [&rootKeys](UIEvent&) { ++rootKeys; });
    t.Group->SetEnabled(false);
    t.Frame();

    t.Click(*t.Control);
    EXPECT_FALSE(t.Control->IsChecked());
    EXPECT_EQ(rootDowns, 1) << "the press did not reach the ancestor outside the disabled group";
    t.Ui->OnKey(Input::kKeyCode_Escape, GLFW_PRESS, 0);
    EXPECT_EQ(rootKeys, 1) << "the key did not reach the ancestor outside the disabled group";
}

// The wheel still reaches a disabled subtree: scrolling only reads, so a scroll view inside a
// disabled group scrolls its content, as a disabled fieldset's contents do in a browser.
TEST(DisabledControlInputTests, AScrollViewInsideADisabledGroupStillScrollsOnTheWheel)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    UiRgHarness rg(dev);
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto group = std::make_unique<UIElement>();
    group->SetId("group");
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->SetId("scroll");
    auto tall = std::make_unique<UIElement>();
    tall->SetId("tall");
    scrollView->AddContent(std::move(tall));
    ScrollView* scroll = scrollView.get();
    group->AddChild(std::move(scrollView));
    UIElement* groupEl = group.get();
    root->AddChild(std::move(group));
    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    Stylesheet sheet{};
    const char* css = R"(
#root { width: 400px; height: 100px; }
#group { width: 200px; height: 80px; }
#scroll { width: 200px; height: 60px; }
#tall { width: 100px; height: 600px; }
)";
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
    groupEl->SetEnabled(false);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);

    ui.OnMouseMove(scroll->GetLayoutX() + 5.0f, scroll->GetLayoutY() + 5.0f);
    ui.Update(0.0f, /*interactive=*/true);
    DriveUiRender(ui, rg);
    ui.OnScroll(0.0f, -1.0f);
    EXPECT_GT(scroll->GetScrollY(), 0.0f) << "a scroll view inside a disabled group did not scroll on the wheel";
}

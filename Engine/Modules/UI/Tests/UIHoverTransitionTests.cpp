#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "Assets/AssetManager.h"
#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

TEST(UIManagerHoverTests, MouseEnterAndLeaveOnlyOnTransitions)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_hover_transitions.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 100px; height: 100px; }
#a { width: 40px; height: 40px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);

    int rootEnterCount = 0;
    int rootLeaveCount = 0;
    int childEnterCount = 0;
    int childLeaveCount = 0;

    r->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++rootEnterCount; });
    r->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++rootLeaveCount; });
    a->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++childEnterCount; });
    a->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++childLeaveCount; });

    // Start with pointer far outside the layout: no hover on first frame.
    ui.OnMouseMove(-1000.0f, -1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 0);
    EXPECT_EQ(rootLeaveCount, 0);
    EXPECT_EQ(childEnterCount, 0);
    EXPECT_EQ(childLeaveCount, 0);

    // Move pointer into the child: one MouseEnter on child and root.
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 0);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 0);

    // Move pointer around inside the same element: no additional enter/leave.
    ui.OnMouseMove(15.0f, 15.0f);
    ui.Update(0.0f, /*interactive=*/true);
    ui.OnMouseMove(20.0f, 20.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 0);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 0);

    // Move pointer well outside all elements: one MouseLeave on child and root.
    ui.OnMouseMove(1000.0f, 1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 1);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 1);

    // Further frames with pointer still outside should not generate more events.
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 1);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 1);
}

TEST(UIManagerHoverTests, CursorLeaveDispatchesMouseLeave)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_hover_cursor_leave.css";
    {
        std::ofstream f(css);
        f << R"(
#root { display: flex; width: 100px; height: 100px; }
#a { width: 40px; height: 40px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);

    int rootEnterCount = 0;
    int rootLeaveCount = 0;
    int childEnterCount = 0;
    int childLeaveCount = 0;

    r->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++rootEnterCount; });
    r->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++rootLeaveCount; });
    a->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++childEnterCount; });
    a->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++childLeaveCount; });

    // Enter the child.
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 0);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 0);

    // Simulate OS cursor leaving the window: should dispatch MouseLeave on the next Update().
    ui.OnCursorEnter(false);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 1);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 1);

    // Further frames while still outside should not generate more events.
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(rootEnterCount, 1);
    EXPECT_EQ(rootLeaveCount, 1);
    EXPECT_EQ(childEnterCount, 1);
    EXPECT_EQ(childLeaveCount, 1);
}

TEST(UIManagerHoverTests, PointerEventsNoneIsIgnoredForHover)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    // Two overlapping elements: an overlay that covers the root, but with pointer-events:none.
    // Hover should target the underlying element, not the overlay.
    const std::string xml = R"(<uielement id='root'>
        <uielement id='under' />
        <uielement id='overlay' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto css = tmpDir / "ui_pointer_events_hover.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 100px; height: 100px; position: relative; }
#under { width: 100px; height: 100px; }
#overlay { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; z-index: 1000; pointer-events: none; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* under = r->FindById("under");
    UIElement* overlay = r->FindById("overlay");
    ASSERT_NE(under, nullptr);
    ASSERT_NE(overlay, nullptr);

    int underEnter = 0, underLeave = 0;
    int overlayEnter = 0, overlayLeave = 0;

    under->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++underEnter; });
    under->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++underLeave; });
    overlay->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++overlayEnter; });
    overlay->RegisterEventHandler(kEventMouseLeave, [&](UIEvent&) { ++overlayLeave; });

    // Move into the overlap area: should hover 'under', not 'overlay'.
    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(underEnter, 1);
    EXPECT_EQ(overlayEnter, 0);

    // Move out: should leave 'under'. Overlay should still never receive hover events.
    ui.OnMouseMove(1000.0f, 1000.0f);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(underLeave, 1);
    EXPECT_EQ(overlayLeave, 0);
}

TEST(UIManagerHoverTests, OverlayLayerPriorityMatchesRenderOrdering)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    // The modal is deliberately earlier in traversal order. A later ordinary
    // sibling must not receive pointer input through an overlay drawn above it.
    const std::string xml = R"(<uielement id='root'>
        <uielement id='modal' />
        <uielement id='ordinary' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIElement* modal = root->FindById("modal");
    ASSERT_NE(modal, nullptr);
    modal->SetOverlayLayer(OverlayLayer::Modal);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_overlay_hit_order.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 100px; height: 100px; position: relative; }
#modal, #ordinary { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* ordinary = ui.GetRootElement()->FindById("ordinary");
    ASSERT_NE(ordinary, nullptr);

    int modalEnter = 0;
    int ordinaryEnter = 0;
    modal->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++modalEnter; });
    ordinary->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++ordinaryEnter; });

    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(modalEnter, 1);
    EXPECT_EQ(ordinaryEnter, 0);
}

TEST(UIManagerHoverTests, NestedDropdownKeepsContainingModalPriority)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <uielement id='modal'>
            <uielement id='backdrop' />
            <uielement id='dropdown-popup' />
        </uielement>
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIElement* modal = root->FindById("modal");
    UIElement* popup = root->FindById("dropdown-popup");
    ASSERT_NE(modal, nullptr);
    ASSERT_NE(popup, nullptr);
    modal->SetOverlayLayer(OverlayLayer::Modal);
    popup->SetOverlayLayer(OverlayLayer::Dropdown);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_nested_overlay_hit_order.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 100px; height: 100px; position: relative; }
#modal, #backdrop, #dropdown-popup { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* backdrop = ui.GetRootElement()->FindById("backdrop");
    ASSERT_NE(backdrop, nullptr);
    int backdropEnter = 0;
    int popupEnter = 0;
    backdrop->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++backdropEnter; });
    popup->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++popupEnter; });

    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(popupEnter, 1);
    EXPECT_EQ(backdropEnter, 0);
}

TEST(UIManagerHoverTests, BlockingDialogOutranksLaterModalFloatingPanel)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    // Dialog is earlier in the tree. The floating panel is a later Modal
    // sibling — the torn-off Game View case. Overlay layer, not sibling
    // order, must decide paint and hit-test.
    const std::string xml = R"(<uielement id='root'>
        <uielement id='dialog' />
        <uielement id='floating' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIElement* dialog = root->FindById("dialog");
    UIElement* floating = root->FindById("floating");
    ASSERT_NE(dialog, nullptr);
    ASSERT_NE(floating, nullptr);
    dialog->SetOverlayLayer(OverlayLayer::BlockingDialog);
    floating->SetOverlayLayer(OverlayLayer::Modal);

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto css = std::filesystem::temp_directory_path() / "ui_blocking_dialog_hit_order.css";
    {
        std::ofstream f(css);
        f << R"(
#root { width: 100px; height: 100px; position: relative; }
#dialog, #floating { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(css.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    dialog = ui.GetRootElement()->FindById("dialog");
    floating = ui.GetRootElement()->FindById("floating");
    int dialogEnter = 0;
    int floatingEnter = 0;
    dialog->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++dialogEnter; });
    floating->RegisterEventHandler(kEventMouseEnter, [&](UIEvent&) { ++floatingEnter; });

    ui.OnMouseMove(10.0f, 10.0f);
    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(dialogEnter, 1);
    EXPECT_EQ(floatingEnter, 0);
}

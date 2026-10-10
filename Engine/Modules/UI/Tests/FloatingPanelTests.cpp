#include <gtest/gtest.h>

#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Application.h"
#include "Input/InputSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/FloatingPanel.h"
#include "UI/Controls/Label.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace GameEngine;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

std::string ReadStagedSheet(const char* relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A UIManager over the staged Assets/ tree, mounted as both the project and
// the editor source: FloatingPanel instantiates its chrome from
// UI/controls/FloatingPanel.uxml and styles it from FloatingPanel.css through
// the asset manager, so a frame only grows its title bar, close button and
// grip once it is owned by a manager that can resolve those paths.
class FloatingPanelChromeFixture
{
public:
    bool Init(std::string* why)
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
        {
            *why = "Device init failed";
            return false;
        }
        UIRegistration::RegisterBuiltInControls();

        namespace fs = std::filesystem;
        const fs::path assetRoot = PathUtils::GetExecutableDirectory() / "Assets";
        const fs::path scratch = fs::temp_directory_path() / "ui_floating_panel_tests";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch, ec);

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        if (!m_Assets.Initialize(assetRoot, m_Pool.get(), scratch / "AssetDatabase.assetdb",
                                 scratch / ".Cache" / "AssetDatabase"))
        {
            *why = "AssetManager init failed";
            return false;
        }
        AssetSourceDesc editorSource{};
        editorSource.Alias = std::string(kAssetSourceAliasEditor);
        editorSource.Root = assetRoot;
        if (!m_Assets.RegisterSource(editorSource))
        {
            *why = "editor source registration failed";
            return false;
        }
        m_Assets.WaitForStartupScan(kAssetSourceAliasProject);
        m_Assets.WaitForStartupScan(kAssetSourceAliasEditor);

        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        m_Ui = std::make_unique<UIManager>(m_Device, &m_Assets);
        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        m_Ui->SetRoot(std::move(root));

        // The dockspace type scale comes from the theme, which the editor
        // loads globally; the frame sheet comes through the control itself.
        const std::string tokens = ReadStagedSheet("theme/tokens.css");
        const std::string core = ReadStagedSheet("theme/core.css");
        if (tokens.empty() || core.empty())
        {
            *why = "shipped stylesheets not staged next to the test exe";
            return false;
        }
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(
                tokens + "\n" + core + "\n#root { width: 800px; height: 600px; position: relative; }\n", sheet))
        {
            *why = "theme stylesheet did not parse";
            return false;
        }
        m_Ui->AddStylesheet(std::make_shared<const Stylesheet>(sheet));
        Settle();
        return true;
    }

    UIElement& Root() { return *m_Ui->GetRootElement(); }
    UIManager& Manager() { return *m_Ui; }

    FloatingPanel* AddFrame()
    {
        auto frame = std::make_unique<FloatingPanel>();
        FloatingPanel* raw = frame.get();
        Root().AddChild(std::move(frame));
        return raw;
    }

    // Enough frames for the chrome bind (one safe point) and a layout pass.
    void Settle(int frames = 3)
    {
        for (int i = 0; i < frames; ++i)
        {
            m_Ui->Update(0.016f, /*interactive=*/true);
            DriveUiRender(*m_Ui, *m_Rg);
        }
    }

private:
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    Rendering::IDevice* m_Device = nullptr;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    AssetManager m_Assets;
    std::unique_ptr<UiRgHarness> m_Rg;
    std::unique_ptr<UIManager> m_Ui;
};

#define FLOATING_PANEL_CHROME_FIXTURE(fx)                                                                        \
    FloatingPanelChromeFixture fx;                                                                               \
    {                                                                                                            \
        std::string why;                                                                                         \
        if (!fx.Init(&why))                                                                                      \
            GTEST_SKIP() << why;                                                                                 \
    }

void SendTo(UIElement* target, EventId id, float x, float y, int mods = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = 0;
    e.ButtonDown = id != kEventMouseUp;
    e.Mods = mods;
    e.Target = target;
    e.CurrentTarget = target;
    target->DispatchEvent(e);
}

} // namespace

TEST(FloatingPanelTests, ShowMountsPanelAndHideUnmounts)
{
    FloatingPanel frame;
    UIElement panel;
    frame.Show("Hierarchy", &panel, "Hierarchy", 10.0f, 20.0f, 400.0f, 300.0f);
    EXPECT_TRUE(frame.IsOpen());
    EXPECT_EQ(frame.GetPanelId(), "Hierarchy");
    EXPECT_EQ(frame.GetMountedPanel(), &panel);

    frame.Hide();
    EXPECT_FALSE(frame.IsOpen());
    EXPECT_TRUE(frame.GetPanelId().empty());
    EXPECT_EQ(frame.GetMountedPanel(), nullptr);
}

TEST(FloatingPanelTests, ShowUsesDockMountIdBeforeAndAfterChrome)
{
    FloatingPanel bare;
    UIElement panel;
    bare.Show("SceneView", &panel, "Scene View", 0.0f, 0.0f, 400.0f, 300.0f);
    ASSERT_NE(bare.FindById("mount:SceneView"), nullptr);
    bare.Hide();
    EXPECT_EQ(bare.FindById("mount:SceneView"), nullptr);

    FLOATING_PANEL_CHROME_FIXTURE(fx);
    FloatingPanel* frame = fx.AddFrame();
    frame->Show("SceneView", &panel, "Scene View", 0.0f, 0.0f, 400.0f, 300.0f);
    fx.Settle();
    UIElement* mount = frame->FindById("mount:SceneView");
    ASSERT_NE(mount, nullptr);
    UIElement* body = frame->FindById("floating-panel-body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(mount->GetParent(), body);
    EXPECT_EQ(frame->GetMountedPanel(), &panel);
}

TEST(FloatingPanelTests, ChromeComesFromTheLayoutAsset)
{
    UIElement panel;
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    FloatingPanel* frame = fx.AddFrame();
    EXPECT_EQ(frame->FindById("floating-panel-titlebar"), nullptr);
    fx.Settle();

    UIElement* titleBar = frame->FindById("floating-panel-titlebar");
    ASSERT_NE(titleBar, nullptr);
    EXPECT_TRUE(titleBar->HasClass("floating-panel-titlebar"));
    ASSERT_NE(dynamic_cast<Label*>(frame->FindById("floating-panel-title")), nullptr);
    ASSERT_NE(dynamic_cast<Button*>(frame->FindById("floating-panel-close")), nullptr);
    ASSERT_NE(frame->FindById("floating-panel-body"), nullptr);
    ASSERT_NE(frame->FindById("floating-panel-resize"), nullptr);

    // A frame is display:none until Show(); only a shown frame gets a layout.
    // The frame's own sheet reached the subtree: the title bar has its height.
    frame->Show("SceneView", &panel, "Scene View", 0.0f, 0.0f, 400.0f, 300.0f);
    fx.Settle();
    EXPECT_FLOAT_EQ(titleBar->GetLayoutHeight(), 28.0f);
}

TEST(FloatingPanelTests, ShowCopiesDockPanelTabIcon)
{
    class IconPanel : public DockPanel
    {
    public:
        IconPanel() : DockPanel("Inspector") {}
        std::string_view DeclaredTabIconClass() const override { return "dock-inspector-icon"; }
    };

    FLOATING_PANEL_CHROME_FIXTURE(fx);
    IconPanel panel;
    FloatingPanel* frame = fx.AddFrame();
    frame->Show("Inspector", &panel, "Inspector", 0.0f, 0.0f, 400.0f, 300.0f);
    fx.Settle();

    UIElement* icon = frame->FindById("floating-panel-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("dock-tab-icon"));
    EXPECT_TRUE(icon->HasClass("dock-inspector-icon"));
    EXPECT_FALSE(icon->HasClass("floating-panel-icon-hidden"));
    auto* title = dynamic_cast<Label*>(frame->FindById("floating-panel-title"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->GetText(), "Inspector");

    frame->Hide();
    EXPECT_FALSE(icon->HasClass("dock-inspector-icon"));
    EXPECT_TRUE(icon->HasClass("floating-panel-icon-hidden"));
}

TEST(FloatingPanelTests, CloseButtonInvokesCallback)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panel;
    FloatingPanel* frame = fx.AddFrame();
    int closes = 0;
    frame->SetOnClose([&]() { ++closes; });
    frame->Show("Inspector", &panel, "Inspector", 0.0f, 0.0f, 400.0f, 300.0f);
    fx.Settle();

    auto* close = dynamic_cast<Button*>(frame->FindById("floating-panel-close:Inspector"));
    ASSERT_NE(close, nullptr);
    close->TriggerClick();
    EXPECT_EQ(closes, 1);
    EXPECT_TRUE(frame->IsOpen());
}

TEST(FloatingPanelTests, CloseWithoutCallbackHides)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panel;
    FloatingPanel* frame = fx.AddFrame();
    frame->Show("Log", &panel, "Log", 0.0f, 0.0f, 400.0f, 300.0f);
    fx.Settle();
    auto* close = dynamic_cast<Button*>(frame->FindById("floating-panel-close:Log"));
    ASSERT_NE(close, nullptr);
    close->TriggerClick();
    EXPECT_FALSE(frame->IsOpen());
    EXPECT_EQ(frame->GetMountedPanel(), nullptr);
}

TEST(FloatingPanelTests, ShowClampsInitialPositionInsideParent)
{
    UIElement host;
    UIElement panel;
    UILayoutAccess::SetLastLayoutRect(host, 0.0f, 0.0f, 800.0f, 600.0f);

    auto frame = std::make_unique<FloatingPanel>();
    FloatingPanel* raw = frame.get();
    host.AddChild(std::move(frame));
    raw->Show("Inspector", &panel, "Inspector", 2400.0f, 1800.0f, 400.0f, 300.0f);

    const auto left = raw->Overrides().Get(Style::PositionLeft);
    const auto top = raw->Overrides().Get(Style::PositionTop);
    const auto width = raw->Overrides().Get(Style::Width);
    const auto height = raw->Overrides().Get(Style::Height);
    ASSERT_TRUE(left.has_value() && left->IsPx());
    ASSERT_TRUE(top.has_value() && top->IsPx());
    ASSERT_TRUE(width.has_value() && width->IsPx());
    ASSERT_TRUE(height.has_value() && height->IsPx());
    EXPECT_FLOAT_EQ(left->Value, 392.0f);
    EXPECT_FLOAT_EQ(top->Value, 292.0f);
    EXPECT_FLOAT_EQ(width->Value, 400.0f);
    EXPECT_FLOAT_EQ(height->Value, 300.0f);
}

TEST(FloatingPanelTests, DragAllowsHangingOffLeftEdge)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panel;
    FloatingPanel* raw = fx.AddFrame();
    raw->Show("Inspector", &panel, "Inspector", 100.0f, 80.0f, 400.0f, 300.0f);
    fx.Settle();
    ASSERT_FLOAT_EQ(raw->GetLayoutWidth(), 400.0f);

    UIElement* titleBar = raw->FindById("floating-panel-titlebar:Inspector");
    ASSERT_NE(titleBar, nullptr);
    SendTo(titleBar, kEventMouseDown, 180.0f, 90.0f);
    SendTo(titleBar, kEventMouseMove, -400.0f, 90.0f);

    const auto left = raw->Overrides().Get(Style::PositionLeft);
    ASSERT_TRUE(left.has_value() && left->IsPx());
    EXPECT_FLOAT_EQ(left->Value, -328.0f);
}

TEST(FloatingPanelTests, TornOffPanelMatchesDockspaceFontSize)
{
    const std::string tokens = ReadStagedSheet("theme/tokens.css");
    const std::string core = ReadStagedSheet("theme/core.css");
    const std::string frameSheet = ReadStagedSheet("controls/FloatingPanel.css");
    ASSERT_FALSE(tokens.empty() || core.empty() || frameSheet.empty())
        << "shipped stylesheets not staged next to the test exe";

    IsolatedUIFixture fx;
    constexpr const char* kXml = R"(<uielement id="root">
  <label id="root-child">Aa</label>
  <uielement class="dockspace">
    <label id="docked">Aa</label>
  </uielement>
  <uielement id="dock-box" class="dock-content">
    <label id="dock-child">Aa</label>
  </uielement>
  <uielement id="torn-frame" class="floating-panel">
    <uielement id="torn-body" class="floating-panel-body">
      <label id="torn">Aa</label>
    </uielement>
  </uielement>
</uielement>)";
    if (!fx.Build(1.0f, kXml, tokens + "\n" + core + "\n" + frameSheet))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const ResolvedStyle* docked = fx.Style("docked");
    const ResolvedStyle* torn = fx.Style("torn");
    const ResolvedStyle* frame = fx.Style("torn-frame");
    ASSERT_NE(docked, nullptr);
    ASSERT_NE(torn, nullptr);
    ASSERT_NE(frame, nullptr);
    EXPECT_FLOAT_EQ(torn->Visual.FontSize, docked->Visual.FontSize);
    EXPECT_FLOAT_EQ(docked->Visual.FontSize, 14.0f);
    const ResolvedStyle* rootChild = fx.Style("root-child");
    ASSERT_NE(rootChild, nullptr);
    EXPECT_FLOAT_EQ(rootChild->Visual.FontSize, 14.0f);

    const CornerRadiiTLTRBRBL radii = UsedBorderRadius(frame->Visual, 400.0f, 300.0f);
    EXPECT_FLOAT_EQ(radii.TopLeft.X, 8.0f);
    EXPECT_FLOAT_EQ(radii.TopRight.X, 8.0f);
    EXPECT_FLOAT_EQ(radii.BottomRight.X, 8.0f);
    EXPECT_FLOAT_EQ(radii.BottomLeft.X, 8.0f);
    EXPECT_FLOAT_EQ(radii.TopLeft.Y, 8.0f);
    EXPECT_FLOAT_EQ(radii.TopRight.Y, 8.0f);
    EXPECT_FLOAT_EQ(radii.BottomRight.Y, 8.0f);
    EXPECT_FLOAT_EQ(radii.BottomLeft.Y, 8.0f);
    const CornerRadiiTLTRBRBL largeRadii = UsedBorderRadius(frame->Visual, 1600.0f, 900.0f);
    EXPECT_FLOAT_EQ(largeRadii.TopLeft.X, 8.0f);
    EXPECT_FLOAT_EQ(largeRadii.BottomRight.X, 8.0f);
    EXPECT_EQ(frame->Layout.Overflow, Overflow::Visible);

    const ResolvedStyle* body = fx.Style("torn-body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->Layout.Overflow, Overflow::Hidden);
    const CornerRadiiTLTRBRBL bodySmall = UsedBorderRadius(body->Visual, 400.0f, 300.0f);
    const CornerRadiiTLTRBRBL bodyLarge = UsedBorderRadius(body->Visual, 1600.0f, 900.0f);
    EXPECT_FLOAT_EQ(bodySmall.TopLeft.X, 0.0f);
    EXPECT_FLOAT_EQ(bodySmall.BottomLeft.X, 8.0f);
    EXPECT_FLOAT_EQ(bodySmall.BottomRight.X, 8.0f);
    EXPECT_FLOAT_EQ(bodyLarge.BottomLeft.X, 8.0f);
    EXPECT_FLOAT_EQ(bodyLarge.BottomRight.X, 8.0f);

    const ResolvedStyle* dockBox = fx.Style("dock-box");
    ASSERT_NE(dockBox, nullptr);
    const CornerRadiiTLTRBRBL dockSmall = UsedBorderRadius(dockBox->Visual, 400.0f, 300.0f);
    const CornerRadiiTLTRBRBL dockLarge = UsedBorderRadius(dockBox->Visual, 1600.0f, 900.0f);
    EXPECT_FLOAT_EQ(dockSmall.TopLeft.X, 0.0f);
    EXPECT_FLOAT_EQ(dockSmall.BottomLeft.X, 6.0f);
    EXPECT_FLOAT_EQ(dockSmall.BottomRight.X, 6.0f);
    EXPECT_FLOAT_EQ(dockLarge.BottomLeft.X, 6.0f);
    EXPECT_FLOAT_EQ(dockLarge.BottomRight.X, 6.0f);
}

TEST(FloatingPanelTests, OverlayMountedPanelMatchesDockspaceFontSize)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);

    auto dock = std::make_unique<UIElement>();
    dock->AddClass("dockspace");
    auto dockedLabel = std::make_unique<Label>();
    dockedLabel->SetId("docked");
    dockedLabel->SetText("Aa");
    UIElement* dockedRaw = dockedLabel.get();
    dock->AddChild(std::move(dockedLabel));
    fx.Root().AddChild(std::move(dock));

    auto panel = std::make_unique<UIElement>();
    auto mounted = std::make_unique<Label>();
    mounted->SetId("torn-mounted");
    mounted->SetText("Aa");
    UIElement* mountedRaw = mounted.get();
    panel->AddChild(std::move(mounted));

    FloatingPanel* frame = fx.AddFrame();
    frame->Show("Inspector", panel.get(), "Inspector", 20.0f, 20.0f, 400.0f, 300.0f);
    fx.Settle();

    const ResolvedStyle& docked = dockedRaw->GetResolvedStyle();
    EXPECT_FLOAT_EQ(docked.Visual.FontSize, 14.0f);
    EXPECT_FLOAT_EQ(mountedRaw->GetResolvedStyle().Visual.FontSize, docked.Visual.FontSize);
    EXPECT_EQ(frame->GetOverlayLayer(), OverlayLayer::Modal);

    UIElement* title = frame->FindById("floating-panel-title");
    ASSERT_NE(title, nullptr);
    EXPECT_FLOAT_EQ(title->GetResolvedStyle().Visual.FontSize, docked.Visual.FontSize);
}

TEST(FloatingPanelTests, OverlayLayerStaysBelowBlockingDialogs)
{
    FloatingPanel frame;
    EXPECT_EQ(frame.GetOverlayLayer(), OverlayLayer::Modal);
    EXPECT_LT(static_cast<uint8_t>(OverlayLayer::Modal),
              static_cast<uint8_t>(OverlayLayer::BlockingDialog));
}

TEST(FloatingPanelTests, ChromeIdsAreUniquePerOpenFrame)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panelA;
    UIElement panelB;
    FloatingPanel* frameA = fx.AddFrame();
    FloatingPanel* frameB = fx.AddFrame();
    frameA->Show("Hierarchy", &panelA, "Hierarchy", 0.0f, 0.0f, 400.0f, 300.0f);
    frameB->Show("Inspector", &panelB, "Inspector", 40.0f, 40.0f, 400.0f, 300.0f);
    fx.Settle();

    UIElement& host = fx.Root();
    EXPECT_EQ(host.FindById("floating-panel-titlebar"), nullptr);
    ASSERT_NE(host.FindById("floating-panel-titlebar:Hierarchy"), nullptr);
    ASSERT_NE(host.FindById("floating-panel-titlebar:Inspector"), nullptr);
    EXPECT_NE(host.FindById("floating-panel-titlebar:Hierarchy"),
              host.FindById("floating-panel-titlebar:Inspector"));
    EXPECT_EQ(frameA->GetId(), "floating:Hierarchy");
    EXPECT_EQ(frameB->GetId(), "floating:Inspector");
}

TEST(FloatingPanelTests, AltDragMoveReportsModifierThenEnd)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panel;
    FloatingPanel* raw = fx.AddFrame();
    raw->Show("Inspector", &panel, "Inspector", 100.0f, 80.0f, 400.0f, 300.0f);
    fx.Settle();

    int moves = 0;
    int ends = 0;
    bool lastAlt = false;
    bool lastCancelled = true;
    raw->SetOnTitleDragMove([&](float, float, bool altHeld)
                            {
                                ++moves;
                                lastAlt = altHeld;
                            });
    raw->SetOnTitleDragEnd([&](float, float, bool altHeld, bool cancelled)
                           {
                               ++ends;
                               lastAlt = altHeld;
                               lastCancelled = cancelled;
                           });

    UIElement* titleBar = raw->FindById("floating-panel-titlebar:Inspector");
    ASSERT_NE(titleBar, nullptr);
    SendTo(titleBar, kEventMouseDown, 180.0f, 90.0f, Input::kModAlt);
    SendTo(titleBar, kEventMouseMove, 200.0f, 90.0f, Input::kModAlt);
    EXPECT_EQ(moves, 1);
    EXPECT_TRUE(lastAlt);
    EXPECT_TRUE(raw->IsTitleDragging());
    SendTo(titleBar, kEventMouseUp, 200.0f, 90.0f, Input::kModAlt);
    EXPECT_EQ(ends, 1);
    EXPECT_TRUE(lastAlt);
    EXPECT_FALSE(lastCancelled);
    EXPECT_FALSE(raw->IsTitleDragging());
}

TEST(FloatingPanelTests, MouseCancelEndsTitleDragWithoutCommit)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panel;
    FloatingPanel* raw = fx.AddFrame();
    raw->Show("Inspector", &panel, "Inspector", 100.0f, 80.0f, 400.0f, 300.0f);
    fx.Settle();

    bool cancelled = false;
    raw->SetOnTitleDragEnd([&](float, float, bool, bool wasCancelled) { cancelled = wasCancelled; });

    UIElement* titleBar = raw->FindById("floating-panel-titlebar:Inspector");
    ASSERT_NE(titleBar, nullptr);
    SendTo(titleBar, kEventMouseDown, 180.0f, 90.0f);
    EXPECT_TRUE(raw->IsTitleDragging());

    UIEvent cancel{};
    cancel.Id = kEventMouseCancel;
    cancel.Target = titleBar;
    titleBar->DispatchEvent(cancel);
    EXPECT_FALSE(raw->IsTitleDragging());
    EXPECT_TRUE(cancelled);
}

TEST(FloatingPanelTests, MouseUpOverSecondFrameReleasesTitleDrag)
{
    FLOATING_PANEL_CHROME_FIXTURE(fx);
    UIElement panelA;
    UIElement panelB;
    FloatingPanel* frameA = fx.AddFrame();
    FloatingPanel* frameB = fx.AddFrame();
    frameA->Show("Hierarchy", &panelA, "Hierarchy", 20.0f, 20.0f, 360.0f, 280.0f);
    frameB->Show("Inspector", &panelB, "Inspector", 200.0f, 40.0f, 360.0f, 280.0f);
    fx.Settle();

    UIManager& ui = fx.Manager();
    // Press on A's title strip, drag onto B, release. Capture must leave with
    // A's drag ended even though B is the later Modal sibling under the cursor.
    ui.OnMouseMove(40.0f, 34.0f);
    ui.Update(0.016f, /*interactive=*/true);
    ui.OnMouseButton(0, true);
    ui.Update(0.016f, /*interactive=*/true);
    EXPECT_TRUE(frameA->IsTitleDragging());
    EXPECT_EQ(ui.GetCaptureId(), "floating:Hierarchy");

    ui.OnMouseMove(280.0f, 54.0f);
    ui.Update(0.016f, /*interactive=*/true);
    ui.OnMouseButton(0, false);
    ui.Update(0.016f, /*interactive=*/true);

    EXPECT_FALSE(frameA->IsTitleDragging());
    EXPECT_FALSE(frameB->IsTitleDragging());
    EXPECT_TRUE(ui.GetCaptureId().empty());

    const auto leftAfterUp = frameA->Overrides().Get(Style::PositionLeft);
    ASSERT_TRUE(leftAfterUp.has_value() && leftAfterUp->IsPx());
    const float leftAtRelease = leftAfterUp->Value;

    ui.OnMouseMove(500.0f, 80.0f);
    ui.Update(0.016f, /*interactive=*/true);
    const auto leftAfterMove = frameA->Overrides().Get(Style::PositionLeft);
    ASSERT_TRUE(leftAfterMove.has_value() && leftAfterMove->IsPx());
    EXPECT_FLOAT_EQ(leftAfterMove->Value, leftAtRelease);
}

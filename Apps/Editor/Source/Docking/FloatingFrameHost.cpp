#include "Docking/FloatingFrameHost.h"

#include "Core/DeferredActionQueue.h"
#include "EditorPanelManager.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/FloatingPanel.h"
#include "UI/Layout/Docking.h"
#include "UI/Layout/DockingHitTest.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine
{
namespace Editor
{
namespace
{
// Frame size when the panel's dock leaf cannot be measured.
constexpr float kDefaultFrameWidthPx = 960.0f;
constexpr float kDefaultFrameHeightPx = 600.0f;
constexpr float kMinFrameWidthPx = 320.0f;
constexpr float kMinFrameHeightPx = 240.0f;
// A measured extent below this is a collapsed or unlaid-out box, not a size.
constexpr float kMinMeasuredExtentPx = 50.0f;
// Cursor offset into the title bar, the same anchor the OS tear-off uses so
// the frame appears under the pointer the way a torn-off window does.
constexpr float kAnchorX = 16.0f;
constexpr float kAnchorY = 12.0f;

DockspaceElement* FindMainDockspace(FloatingFrameHost::EditorWindowContext* ctx)
{
    if (!ctx || !ctx->ui)
        return nullptr;
    UIElement* root = ctx->ui->GetRootElement();
    if (!root)
        return nullptr;
    return dynamic_cast<DockspaceElement*>(root->FindById("dock"));
}

void ApplyDockDrop(DockingManager& docking, const DockDropTarget& target, const std::string& panelId)
{
    switch (target.Kind)
    {
    case DockDropTarget::TargetKind::Tab:
    case DockDropTarget::TargetKind::TabBar:
        docking.DockAsTabInLeafByPath(target.Path, panelId);
        break;
    case DockDropTarget::TargetKind::LeafSplit:
    case DockDropTarget::TargetKind::RegionSplit:
        docking.DockSplitSubtreeByPath(target.Path, target.Edge, panelId);
        break;
    case DockDropTarget::TargetKind::RootSplit:
        docking.DockToRoot(target.Edge, panelId);
        break;
    case DockDropTarget::TargetKind::None:
        break;
    }
    docking.ActivateTab(panelId);
}

DockDropTarget HitTestDockDrop(DockspaceElement* dock, float mouseX, float mouseY)
{
    DockDropTarget target{};
    if (!dock || !dock->GetModel())
        return target;
    const float dsX = dock->GetLayoutX();
    const float dsY = dock->GetLayoutY();
    const float dsW = dock->GetLayoutWidth();
    const float dsH = dock->GetLayoutHeight();
    const float localX = mouseX - dsX;
    const float localY = mouseY - dsY;
    if (localX < 0.0f || localY < 0.0f || localX >= dsW || localY >= dsH)
        return target;
    return DockingHitTest::Compute(*dock->GetModel(), dsW, dsH, localX, localY);
}

// The dock leaf the panel occupies, so the frame opens at the size the panel
// already had. Active tabs are found through their tab element, inactive ones
// through the mount that holds them.
UIElement* FindPanelLeaf(UIElement& root, const std::string& panelId)
{
    if (UIElement* tabEl = root.FindById(std::string("tab:") + panelId))
    {
        UIElement* p = tabEl;
        while (p && !p->HasClass("dock-leaf"))
            p = p->GetParent();
        if (p)
            return p;
    }
    if (UIElement* mount = root.FindById(std::string("mount:") + panelId))
    {
        UIElement* content = mount->GetParent();
        return content ? content->GetParent() : nullptr;
    }
    return nullptr;
}

void ClearPointerState(FloatingFrameHost::EditorWindowContext* ctx)
{
    if (ctx && ctx->ui)
    {
        ctx->ui->ClearHover();
        ctx->ui->ClearFocus();
    }
}
} // namespace

void FloatingFrameHost::Initialize(Dependencies deps)
{
    m_Deps = std::move(deps);
}

void FloatingFrameHost::QueueTearOff(std::string panelId)
{
    Mathematics::Vector2 cursor;
    if (EditorWindowContext* mainWindow = m_Deps.GetMainWindow(); mainWindow && mainWindow->ui)
        cursor = mainWindow->ui->GetMousePosition();
    const std::string key = std::string("editor.undock:") + panelId;
    m_Deps.PreUiActions->EnqueueUnique(key, [this, panelId = std::move(panelId), cursor]()
                                       { TearOff(panelId, cursor.x, cursor.y); });
}

bool FloatingFrameHost::Raise(const std::string& panelId)
{
    for (FloatingPanel* frame : m_Frames)
    {
        if (frame && frame->IsOpen() && frame->GetPanelId() == panelId)
        {
            frame->Raise();
            return true;
        }
    }
    return false;
}

void FloatingFrameHost::TearOff(const std::string& panelId, float cursorX, float cursorY)
{
    if (Raise(panelId))
        return;

    EditorWindowContext* mainWindow = m_Deps.GetMainWindow();
    if (!mainWindow || !mainWindow->ui || !mainWindow->docking)
    {
        Logger::Log::Error("FloatingFrameHost: no main window to host '{}'", panelId);
        return;
    }
    UIElement* panel = mainWindow->docking->GetPanel(panelId);
    if (!panel)
    {
        Logger::Log::Warning("FloatingFrameHost: unknown panel '{}'; nothing to tear off", panelId);
        return;
    }
    UIElement* root = mainWindow->ui->GetRootElement();
    if (!root)
    {
        Logger::Log::Error("FloatingFrameHost: no UI root to host '{}'", panelId);
        return;
    }

    float desiredW = kDefaultFrameWidthPx;
    float desiredH = kDefaultFrameHeightPx;
    UIElement* measured = FindPanelLeaf(*root, panelId);
    if (!measured)
        measured = panel;
    const float mw = measured->GetLayoutWidth();
    const float mh = measured->GetLayoutHeight();
    if (mw > kMinMeasuredExtentPx && mh > kMinMeasuredExtentPx)
    {
        desiredW = mw;
        desiredH = mh;
    }
    desiredW = std::max(kMinFrameWidthPx, desiredW);
    desiredH = std::max(kMinFrameHeightPx, desiredH);

    mainWindow->docking->RemoveTab(panelId);
    m_Deps.RebuildDockspaceNow(mainWindow, mainWindow->docking);
    ClearPointerState(mainWindow);

    std::string title = panelId;
    if (auto* dockPanel = dynamic_cast<DockPanel*>(panel); dockPanel && !dockPanel->GetTitle().empty())
        title = dockPanel->GetTitle();

    auto frame = std::make_unique<FloatingPanel>();
    FloatingPanel* raw = frame.get();
    raw->SetOnClose([this, raw]() { QueueClose(raw); });
    raw->SetOnTitleDragMove([this](float x, float y, bool altHeld) { UpdateDockPreview(x, y, altHeld); });
    raw->SetOnTitleDragEnd([this, raw](float x, float y, bool altHeld, bool cancelled)
                           { OnTitleDragEnd(raw, x, y, altHeld, cancelled); });
    root->AddChild(std::move(frame));
    m_Frames.push_back(raw);
    raw->Show(panelId, panel, std::move(title), cursorX - kAnchorX, cursorY - kAnchorY, desiredW, desiredH);
}

void FloatingFrameHost::QueueClose(FloatingPanel* frame)
{
    if (!frame)
        return;
    // The close control still holds mouse capture inside Button::OnEvent. The
    // dock rebuild mutates the UI tree (and, for Game View, re-arms GPU views)
    // while that click is in flight, so it waits for the pre-UI safe point.
    const std::string panelId = frame->GetPanelId();
    const std::string key = std::string("editor.redock-in-window:") + (panelId.empty() ? "unknown" : panelId);
    m_Deps.PreUiActions->EnqueueUnique(key, [this, frame]() { Close(frame); });
}

void FloatingFrameHost::Close(FloatingPanel* frame)
{
    if (!frame || std::find(m_Frames.begin(), m_Frames.end(), frame) == m_Frames.end())
        return;

    const std::string panelId = frame->GetPanelId();
    frame->Hide();

    EditorWindowContext* mainWindow = m_Deps.GetMainWindow();
    ClearPointerState(mainWindow);
    if (mainWindow && mainWindow->docking && !panelId.empty())
    {
        DockingManager& docking = *mainWindow->docking;
        if (docking.RestoreLastClosedTab(panelId))
        {
            (void)docking.ActivateTab(panelId);
        }
        else if (DockNode* leaf = EditorPanelManager::FindFirstLeaf(docking.GetRoot()))
        {
            leaf->AddTab(panelId);
            (void)leaf->ActivateTab(panelId);
        }
        m_Deps.RebuildDockspaceNow(mainWindow, mainWindow->docking);
    }

    Unmount(frame, mainWindow);
}

void FloatingFrameHost::UpdateDockPreview(float mouseX, float mouseY, bool altHeld)
{
    DockspaceElement* dock = FindMainDockspace(m_Deps.GetMainWindow());
    if (!dock)
        return;
    if (!altHeld)
    {
        dock->ClearDropPreview();
        return;
    }
    const DockDropTarget target = HitTestDockDrop(dock, mouseX, mouseY);
    if (target.Kind == DockDropTarget::TargetKind::None)
        dock->ClearDropPreview();
    else
        dock->SetDropPreview(target, dock->GetLayoutWidth(), dock->GetLayoutHeight());
}

void FloatingFrameHost::OnTitleDragEnd(FloatingPanel* frame, float mouseX, float mouseY, bool altHeld,
                                       bool cancelled)
{
    if (DockspaceElement* dock = FindMainDockspace(m_Deps.GetMainWindow()))
        dock->ClearDropPreview();
    if (cancelled || !altHeld || !frame)
        return;
    const std::string panelId = frame->GetPanelId();
    const std::string key =
        std::string("editor.redock-in-window-drop:") + (panelId.empty() ? "unknown" : panelId);
    m_Deps.PreUiActions->EnqueueUnique(key, [this, frame, mouseX, mouseY]() { Redock(frame, mouseX, mouseY); });
}

void FloatingFrameHost::Redock(FloatingPanel* frame, float mouseX, float mouseY)
{
    if (!frame || std::find(m_Frames.begin(), m_Frames.end(), frame) == m_Frames.end())
        return;

    EditorWindowContext* mainWindow = m_Deps.GetMainWindow();
    DockspaceElement* dock = FindMainDockspace(mainWindow);
    if (!dock || !mainWindow || !mainWindow->docking)
        return;

    const DockDropTarget target = HitTestDockDrop(dock, mouseX, mouseY);
    if (target.Kind == DockDropTarget::TargetKind::None)
        return;

    const std::string panelId = frame->GetPanelId();
    if (panelId.empty())
        return;

    frame->Hide();
    ClearPointerState(mainWindow);
    ApplyDockDrop(*mainWindow->docking, target, panelId);
    m_Deps.RebuildDockspaceNow(mainWindow, mainWindow->docking);

    Unmount(frame, mainWindow);
}

void FloatingFrameHost::DismissAll()
{
    EditorWindowContext* mainWindow = m_Deps.GetMainWindow();
    ClearPointerState(mainWindow);

    const std::vector<FloatingPanel*> frames = m_Frames;
    for (FloatingPanel* frame : frames)
    {
        if (!frame)
            continue;
        frame->Hide();
        Unmount(frame, mainWindow);
    }
}

void FloatingFrameHost::Unmount(FloatingPanel* frame, EditorWindowContext* mainWindow)
{
    m_Frames.erase(std::remove(m_Frames.begin(), m_Frames.end(), frame), m_Frames.end());
    UIElement* root = (mainWindow && mainWindow->ui) ? mainWindow->ui->GetRootElement() : nullptr;
    if (root && frame->GetParent() == root)
        root->RemoveChild(frame);
}

} // namespace Editor
} // namespace GameEngine

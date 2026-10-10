#include "UI/Controls/FloatingPanel.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Mount.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/InputSystem.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/FloatingPanel.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/FloatingPanel.css";
constexpr const char* kIconHiddenClass = "floating-panel-icon-hidden";
// Same height the stylesheet gives the title bar; the drag clamp keeps this
// much of the frame on screen.
constexpr float kTitleBarHeightPx = 28.0f;
constexpr float kMinWidthPx = 320.0f;
constexpr float kMinHeightPx = 240.0f;
constexpr float kEdgePaddingPx = 8.0f;
constexpr float kMinGrabbableTitlePx = 64.0f;
constexpr int kBaseZIndex = 8000;
int s_TopZIndex = kBaseZIndex;
} // namespace

FloatingPanel::FloatingPanel()
{
    AddClass("floating-panel");
    SetOverlayLayer(OverlayLayer::Modal);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::ZIndex, kBaseZIndex);

    auto mount = std::make_unique<Mount>();
    // Same fill contract as a dock leaf's Mount: scene/game coordinators look up
    // `mount:<panelId>`, and `.mount` is what makes height:100% panels fill.
    mount->AddClass("mount");
    mount->AddClass("floating-panel-mount");
    m_Mount = mount.get();
    AddChild(std::move(mount));

    RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
                         {
                             if (e.Target == m_CloseButton)
                                 return;
                             Raise();
                         });
    RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
                         {
                             UpdateTitleDrag(e);
                             UpdateResize(e);
                         });
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
                         {
                             FinishTitleDrag(&e, false);
                             FinishResize(&e);
                         });
    RegisterEventHandler(kEventMouseCancel, [this](UIEvent& e)
                         {
                             FinishTitleDrag(&e, true);
                             FinishResize(&e);
                         });
}

FloatingPanel::~FloatingPanel()
{
    if (m_Mount)
        m_Mount->SetTarget(nullptr);
}

void FloatingPanel::OnOwnerManagerChanged(UIManager* owner)
{
    if (!owner || m_ChromeBound || m_ChromeBindScheduled)
        return;
    // The layout asset binds through the owning manager, on the next safe
    // point: this runs inside the AddChild that gave the frame its owner.
    m_ChromeBindScheduled = true;
    PostSafeAction([this]() { BindChrome(); });
}

void FloatingPanel::BindChrome()
{
    m_ChromeBindScheduled = false;
    if (m_ChromeBound)
        return;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    if (!ui->InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("FloatingPanel: {} did not instantiate; the frame has no chrome. "
                           "Check that it is staged under the editor asset mount.",
                           kLayoutAssetPath);
        return;
    }

    m_TitleBar = FindById("floating-panel-titlebar");
    m_TitleIcon = FindById("floating-panel-icon");
    m_TitleLabel = dynamic_cast<Label*>(FindById("floating-panel-title"));
    m_CloseButton = dynamic_cast<Button*>(FindById("floating-panel-close"));
    m_BodyHost = FindById("floating-panel-body");
    m_ResizeGrip = FindById("floating-panel-resize");
    if (!m_TitleBar || !m_TitleIcon || !m_TitleLabel || !m_CloseButton || !m_BodyHost || !m_ResizeGrip)
    {
        Logger::Log::Error("FloatingPanel: {} is missing a chrome element; the frame has no chrome.",
                           kLayoutAssetPath);
        return;
    }

    if (auto held = TakeChild(m_Mount))
        m_BodyHost->AddChild(std::move(held));

    m_TitleBar->SetTooltip("Option/Alt-drag onto Editor to dock again");
    m_TitleBar->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) { BeginTitleDrag(e); });
    m_TitleBar->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { UpdateTitleDrag(e); });
    m_TitleBar->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) { FinishTitleDrag(&e, false); });
    m_TitleBar->RegisterEventHandler(kEventMouseCancel, [this](UIEvent& e) { FinishTitleDrag(&e, true); });

    m_CloseButton->SetTooltip("Close");
    m_CloseButton->SetOnClick([this](UIEvent&) { OnCloseClicked(); });

    m_ResizeGrip->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) { BeginResize(e); });
    m_ResizeGrip->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { UpdateResize(e); });
    m_ResizeGrip->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) { FinishResize(&e); });
    m_ResizeGrip->RegisterEventHandler(kEventMouseCancel, [this](UIEvent& e) { FinishResize(&e); });

    m_ChromeBound = true;
    ApplyChromeState();
}

void FloatingPanel::ApplyChromeState()
{
    if (!m_ChromeBound)
        return;
    m_TitleLabel->SetText(m_Open ? m_Title : std::string{});

    if (!m_AppliedIconClass.empty())
        m_TitleIcon->RemoveClass(m_AppliedIconClass);
    m_AppliedIconClass = m_Open ? m_IconClass : std::string{};
    if (m_AppliedIconClass.empty())
    {
        m_TitleIcon->AddClass(kIconHiddenClass);
    }
    else
    {
        m_TitleIcon->RemoveClass(kIconHiddenClass);
        m_TitleIcon->AddClass(m_AppliedIconClass);
    }

    AssignChromeIds(m_Open ? m_PanelId : std::string{});
}

void FloatingPanel::Show(std::string panelId, UIElement* panel, std::string title,
                         float x, float y, float width, float height)
{
    m_PanelId = std::move(panelId);
    SetId(std::string("floating:") + m_PanelId);
    m_Title = title.empty() ? m_PanelId : std::move(title);
    if (auto* dockPanel = dynamic_cast<DockPanel*>(panel))
        m_IconClass = std::string(dockPanel->GetTabIcon());
    else
        m_IconClass.clear();
    // DockspaceElement names leaf mounts `mount:<panelId>`. Scene View / Game
    // View bind their RG textures by that id, not by whichever Mount currently
    // holds the panel. Keep the contract after in-window tear-off.
    m_Mount->SetId(std::string("mount:") + m_PanelId);
    m_Mount->SetTarget(panel);

    m_Open = true;
    Overrides().Set(Style::Display, DisplayMode::Flex);
    ClampFullyInside(x, y, width, height);
    Place(x, y, width, height);
    Raise();
    ApplyChromeState();
    RequestRelayout();
}

void FloatingPanel::Hide()
{
    if (m_Dragging)
        FinishTitleDrag(nullptr, true);
    if (m_Resizing)
        FinishResize(nullptr);
    m_Open = false;
    m_Dragging = false;
    m_Resizing = false;
    m_Mount->SetTarget(nullptr);
    m_Mount->SetId({});
    m_PanelId.clear();
    m_Title.clear();
    m_IconClass.clear();
    ApplyChromeState();
    Overrides().Set(Style::Display, DisplayMode::None);
}

UIElement* FloatingPanel::GetMountedPanel() const
{
    return m_Mount->GetTarget();
}

void FloatingPanel::AssignChromeIds(const std::string& panelId)
{
    // Every open frame has its own chrome ids: capture is restored by id after
    // a tree mutation, and Raise() reparents mid-press.
    auto idFor = [&](const char* prefix) -> std::string
    {
        if (panelId.empty())
            return prefix;
        return std::string(prefix) + ":" + panelId;
    };
    m_TitleBar->SetId(idFor("floating-panel-titlebar"));
    m_CloseButton->SetId(idFor("floating-panel-close"));
    m_ResizeGrip->SetId(idFor("floating-panel-resize"));
}

void FloatingPanel::Raise()
{
    Overrides().Set(Style::ZIndex, ++s_TopZIndex);
    // Reparenting mid-drag changes tree generation; capture is restored by id
    // on the next Update. Stay put until the gesture ends.
    if (m_Dragging || m_Resizing)
        return;
    UIElement* parent = GetParent();
    if (!parent)
        return;
    const auto& siblings = parent->GetChildren();
    if (!siblings.empty() && siblings.back().get() == this)
        return;
    parent->PostSafeAction([parent, this]()
                           {
                               if (!parent || GetParent() != parent)
                                   return;
                               auto held = parent->TakeChild(this);
                               if (held)
                                   parent->AddChild(std::move(held));
                           });
}

void FloatingPanel::Place(float x, float y, float width, float height)
{
    ClampToParent(x, y, width, height);
    Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(x))
        .Set(Style::PositionTop, StyleLength::Px(y))
        .Set(Style::Width, StyleLength::Px(width))
        .Set(Style::Height, StyleLength::Px(height));
    MarkDirty(LayoutDirty | VisualDirty);
}

void FloatingPanel::ClampToParent(float& x, float& y, float& width, float& height) const
{
    width = std::max(width, kMinWidthPx);
    height = std::max(height, kMinHeightPx);

    UIElement* parent = GetParent();
    const float hostW = parent ? parent->GetLayoutWidth() : 0.0f;
    const float hostH = parent ? parent->GetLayoutHeight() : 0.0f;
    if (hostW <= 0.0f || hostH <= 0.0f)
        return;

    width = std::min(width, std::max(kMinWidthPx, hostW - kEdgePaddingPx * 2.0f));
    height = std::min(height, std::max(kMinHeightPx, hostH - kEdgePaddingPx * 2.0f));
    // Keep a title-bar strip on-screen so the frame can always be dragged back.
    // Left/right may hang off the canvas; top stays padded so the bar itself
    // does not leave the window.
    x = std::clamp(x, kEdgePaddingPx - width + kMinGrabbableTitlePx,
                   std::max(kEdgePaddingPx, hostW - kMinGrabbableTitlePx));
    y = std::clamp(y, kEdgePaddingPx, std::max(kEdgePaddingPx, hostH - kTitleBarHeightPx));
}

void FloatingPanel::ClampFullyInside(float& x, float& y, float& width, float& height) const
{
    width = std::max(width, kMinWidthPx);
    height = std::max(height, kMinHeightPx);

    UIElement* parent = GetParent();
    const float hostW = parent ? parent->GetLayoutWidth() : 0.0f;
    const float hostH = parent ? parent->GetLayoutHeight() : 0.0f;
    if (hostW <= 0.0f || hostH <= 0.0f)
        return;

    const float maxW = std::max(1.0f, hostW - kEdgePaddingPx * 2.0f);
    const float maxH = std::max(1.0f, hostH - kEdgePaddingPx * 2.0f);
    width = std::min(width, maxW);
    height = std::min(height, maxH);
    x = std::clamp(x, kEdgePaddingPx, std::max(kEdgePaddingPx, hostW - kEdgePaddingPx - width));
    y = std::clamp(y, kEdgePaddingPx, std::max(kEdgePaddingPx, hostH - kEdgePaddingPx - height));
}

void FloatingPanel::BeginTitleDrag(UIEvent& e)
{
    if (e.Button != 0)
        return;
    if (e.Target == m_CloseButton)
        return;
    Raise();
    m_Dragging = true;
    m_DragMouseStartX = e.X;
    m_DragMouseStartY = e.Y;
    m_DragFrameStartX = GetLayoutX();
    m_DragFrameStartY = GetLayoutY();
    // Capture the frame, not the title bar: the frame id is unique per open
    // frame, so a tree mutation mid-drag restores capture onto this frame.
    e.Capture(this);
    e.Stop();
}

void FloatingPanel::UpdateTitleDrag(UIEvent& e)
{
    if (!m_Dragging)
        return;
    Place(m_DragFrameStartX + (e.X - m_DragMouseStartX),
          m_DragFrameStartY + (e.Y - m_DragMouseStartY),
          GetLayoutWidth() > 0.0f ? GetLayoutWidth() : kMinWidthPx,
          GetLayoutHeight() > 0.0f ? GetLayoutHeight() : kMinHeightPx);
    if (m_OnTitleDragMove)
        m_OnTitleDragMove(e.X, e.Y, IsAltHeld(e));
    e.Stop();
}

void FloatingPanel::FinishTitleDrag(UIEvent* e, bool cancelled)
{
    if (!m_Dragging)
        return;
    m_Dragging = false;
    const float x = e ? e->X : 0.0f;
    const float y = e ? e->Y : 0.0f;
    const bool altHeld = e && IsAltHeld(*e);
    if (m_OnTitleDragEnd)
        m_OnTitleDragEnd(x, y, altHeld, cancelled);
    if (e)
        e->Stop();
}

void FloatingPanel::BeginResize(UIEvent& e)
{
    if (e.Button != 0)
        return;
    Raise();
    m_Resizing = true;
    m_ResizeMouseStartX = e.X;
    m_ResizeMouseStartY = e.Y;
    m_ResizeStartW = GetLayoutWidth();
    m_ResizeStartH = GetLayoutHeight();
    e.Capture(this);
    e.Stop();
}

void FloatingPanel::UpdateResize(UIEvent& e)
{
    if (!m_Resizing)
        return;
    Place(GetLayoutX(), GetLayoutY(),
          m_ResizeStartW + (e.X - m_ResizeMouseStartX),
          m_ResizeStartH + (e.Y - m_ResizeMouseStartY));
    e.Stop();
}

void FloatingPanel::FinishResize(UIEvent* e)
{
    if (!m_Resizing)
        return;
    m_Resizing = false;
    if (e)
        e->Stop();
}

bool FloatingPanel::IsAltHeld(const UIEvent& e)
{
    return (e.Mods & Input::kModAlt) != 0;
}

void FloatingPanel::OnCloseClicked()
{
    if (m_OnClose)
        m_OnClose();
    else
        Hide();
}

} // namespace GameEngine

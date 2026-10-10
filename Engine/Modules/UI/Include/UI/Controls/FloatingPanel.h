#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{

class Button;
class Label;
class Mount;
class UIManager;

/// In-canvas stand-in for a native tool window. Single-surface hosts (web)
/// cannot create a second OS window; it mounts an existing panel with a title
/// bar, close control, and resize grip so tear-off still works.
///
/// The chrome is UI/controls/FloatingPanel.uxml, styled by
/// UI/controls/FloatingPanel.css, instantiated once the frame is owned by a
/// UIManager. Show() may run before that: the title, icon and mounted panel
/// are held as state and applied when the chrome arrives.
class FloatingPanel : public UIElement
{
public:
    FloatingPanel();
    ~FloatingPanel() override;

    void OnOwnerManagerChanged(UIManager* owner) override;

    void Show(std::string panelId, UIElement* panel, std::string title,
              float x, float y, float width, float height);
    void Hide();
    bool IsOpen() const { return m_Open; }

    const std::string& GetPanelId() const { return m_PanelId; }
    UIElement* GetMountedPanel() const;

    void SetOnClose(std::function<void()> cb) { m_OnClose = std::move(cb); }

    /// Title-bar drag. `altHeld` is Option/Alt at the event; drop preview and
    /// redock are the caller's job (the frame has no docking model).
    void SetOnTitleDragMove(std::function<void(float mouseX, float mouseY, bool altHeld)> cb)
    {
        m_OnTitleDragMove = std::move(cb);
    }
    void SetOnTitleDragEnd(std::function<void(float mouseX, float mouseY, bool altHeld, bool cancelled)> cb)
    {
        m_OnTitleDragEnd = std::move(cb);
    }

    /// Paint and hit-test above sibling frames of the same overlay layer.
    void Raise();

    bool IsTitleDragging() const { return m_Dragging; }

private:
    void BindChrome();
    void ApplyChromeState();
    void Place(float x, float y, float width, float height);
    void ClampToParent(float& x, float& y, float& width, float& height) const;
    void ClampFullyInside(float& x, float& y, float& width, float& height) const;
    void AssignChromeIds(const std::string& panelId);
    void BeginTitleDrag(UIEvent& e);
    void UpdateTitleDrag(UIEvent& e);
    void FinishTitleDrag(UIEvent* e, bool cancelled);
    void BeginResize(UIEvent& e);
    void UpdateResize(UIEvent& e);
    void FinishResize(UIEvent* e);
    void OnCloseClicked();
    static bool IsAltHeld(const UIEvent& e);

    std::string m_PanelId;
    std::string m_Title;
    // The hosted panel's tab icon class; empty hides the title icon.
    std::string m_IconClass;
    // The icon class currently on the icon element, removed before the next.
    std::string m_AppliedIconClass;
    bool m_Open = false;
    std::function<void()> m_OnClose;
    std::function<void(float mouseX, float mouseY, bool altHeld)> m_OnTitleDragMove;
    std::function<void(float mouseX, float mouseY, bool altHeld, bool cancelled)> m_OnTitleDragEnd;

    // Chrome, from the layout asset. Null until BindChrome has run.
    bool m_ChromeBound = false;
    bool m_ChromeBindScheduled = false;
    UIElement* m_TitleBar = nullptr;
    UIElement* m_TitleIcon = nullptr;
    Label* m_TitleLabel = nullptr;
    Button* m_CloseButton = nullptr;
    UIElement* m_BodyHost = nullptr;
    UIElement* m_ResizeGrip = nullptr;
    // Built in the constructor so a frame can mount before the chrome exists;
    // re-homed into the body once the chrome is instantiated.
    Mount* m_Mount = nullptr;

    bool m_Dragging = false;
    float m_DragMouseStartX = 0.0f;
    float m_DragMouseStartY = 0.0f;
    float m_DragFrameStartX = 0.0f;
    float m_DragFrameStartY = 0.0f;

    bool m_Resizing = false;
    float m_ResizeMouseStartX = 0.0f;
    float m_ResizeMouseStartY = 0.0f;
    float m_ResizeStartW = 0.0f;
    float m_ResizeStartH = 0.0f;
};

} // namespace GameEngine

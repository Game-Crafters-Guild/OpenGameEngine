#pragma once

#include "UI/UIElement.h"
#include "UI/Layout/Docking.h"

#include <optional>

namespace GameEngine
{

// These are **editor-only** metadata elements used to author docking configuration inside UXML.
// They are parsed once during Editor initialization and then removed when DockspaceElement
// rebuilds itself from the docking model.

class DockConfigElement final : public UIElement
{
public:
    DockConfigElement();
    ~DockConfigElement() override = default;
};

class DockPanelsElement final : public UIElement
{
public:
    DockPanelsElement();
    ~DockPanelsElement() override = default;
};

// Panel inventory entry (metadata): declares panel id/type and optional layout/style assets.
class DockablePanelElement final : public UIElement
{
public:
    DockablePanelElement();
    ~DockablePanelElement() override = default;

    void SetPanelType(const std::string& type) { m_Type = type; }
    const std::string& GetPanelType() const { return m_Type; }

    void SetPanelTitle(const std::string& title) { m_Title = title; }
    const std::string& GetPanelTitle() const { return m_Title; }

    void SetLayoutPath(const std::string& layout) { m_Layout = layout; }
    const std::string& GetLayoutPath() const { return m_Layout; }

    void SetStylePath(const std::string& style) { m_Style = style; }
    const std::string& GetStylePath() const { return m_Style; }

    // Only invoked when the markup carries an icon= attribute, so an engaged
    // optional means "this panel authored an icon decision" and an empty string
    // inside it means the decision was "no icon". Absent leaves the panel type's
    // own declaration in charge.
    void SetTabIcon(const std::string& icon) { m_Icon = icon; }
    const std::optional<std::string>& GetTabIcon() const { return m_Icon; }

    // menu="false" keeps the panel registered and openable by id while hiding
    // it from user-facing panel listings (Window menu, hamburger, search).
    void SetShowInMenu(bool show) { m_ShowInMenu = show; }
    bool GetShowInMenu() const { return m_ShowInMenu; }

private:
    std::string m_Type;
    std::string m_Title;
    std::string m_Layout;
    std::string m_Style;
    std::optional<std::string> m_Icon;
    bool m_ShowInMenu = true;
};

class DockLayoutElement final : public UIElement
{
public:
    DockLayoutElement();
    ~DockLayoutElement() override = default;
};

class DockSplitElement final : public UIElement
{
public:
    DockSplitElement();
    ~DockSplitElement() override = default;

    void SetDirectionFromString(const std::string& dir);
    DockPosition GetDirection() const { return m_Direction; }

    void SetRatio(float ratio);
    float GetRatio() const { return m_Ratio; }

private:
    DockPosition m_Direction = DockPosition::Left;
    float m_Ratio = 0.5f;
};

class DockLeafNodeElement final : public UIElement
{
public:
    DockLeafNodeElement();
    ~DockLeafNodeElement() override = default;

    void SetActiveTab(const std::string& tab) { m_ActiveTab = tab; }
    const std::string& GetActiveTab() const { return m_ActiveTab; }

private:
    std::string m_ActiveTab;
};

class DockTabNodeElement final : public UIElement
{
public:
    DockTabNodeElement();
    ~DockTabNodeElement() override = default;

    void SetPanelId(const std::string& panel) { m_PanelId = panel; }
    const std::string& GetPanelId() const { return m_PanelId; }

private:
    std::string m_PanelId;
};

} // namespace GameEngine



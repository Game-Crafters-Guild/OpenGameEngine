#include "UI/Layout/DockConfigElements.h"

#include "UI/Registration/ElementRegistration.h"

#include <memory>

namespace GameEngine
{

DockConfigElement::DockConfigElement()
{
    AddClass("dock-config");
}

DockPanelsElement::DockPanelsElement()
{
    AddClass("dock-panels");
}

DockablePanelElement::DockablePanelElement()
{
    AddClass("dock-panel");
}

DockLayoutElement::DockLayoutElement()
{
    AddClass("dock-layout");
}

DockSplitElement::DockSplitElement()
{
    AddClass("dock-split");
}

void DockSplitElement::SetDirectionFromString(const std::string& dir)
{
    if (dir == "Left" || dir == "left")
        m_Direction = DockPosition::Left;
    else if (dir == "Right" || dir == "right")
        m_Direction = DockPosition::Right;
    else if (dir == "Top" || dir == "top")
        m_Direction = DockPosition::Top;
    else if (dir == "Bottom" || dir == "bottom")
        m_Direction = DockPosition::Bottom;
}

void DockSplitElement::SetRatio(float ratio)
{
    if (ratio < 0.0f)
        ratio = 0.0f;
    if (ratio > 1.0f)
        ratio = 1.0f;
    m_Ratio = ratio;
}

DockLeafNodeElement::DockLeafNodeElement()
{
    AddClass("dock-leaf");
}

DockTabNodeElement::DockTabNodeElement()
{
    AddClass("dock-tab");
}

} // namespace GameEngine

// Register editor docking config elements with the XML factory registry.
namespace
{
using namespace GameEngine;
using namespace GameEngine::UIRegistration;

static auto s_reg_dockConfig =
    RegisterWithFactory<DockConfigElement>("DockConfig", []()
                                           { return std::make_unique<DockConfigElement>(); })
        .TagAlias("dockconfig");

static auto s_reg_dockPanels =
    RegisterWithFactory<DockPanelsElement>("DockPanels", []()
                                           { return std::make_unique<DockPanelsElement>(); })
        .TagAlias("dockpanels");

static auto s_reg_dockablePanel =
    RegisterWithFactory<DockablePanelElement>("DockablePanel", []()
                                              { return std::make_unique<DockablePanelElement>(); })
        .TagAlias("dockablepanel")
        .Attr("type", &DockablePanelElement::SetPanelType)
        .Attr("title", &DockablePanelElement::SetPanelTitle)
        .Attr("layout", &DockablePanelElement::SetLayoutPath)
        .Attr("style", &DockablePanelElement::SetStylePath)
        .Attr("icon", &DockablePanelElement::SetTabIcon)
        .Attr("menu", &DockablePanelElement::SetShowInMenu);

static auto s_reg_dockLayout =
    RegisterWithFactory<DockLayoutElement>("DockLayout", []()
                                           { return std::make_unique<DockLayoutElement>(); })
        .TagAlias("docklayout");

static auto s_reg_dockSplit =
    RegisterWithFactory<DockSplitElement>("DockSplit", []()
                                          { return std::make_unique<DockSplitElement>(); })
        .TagAlias("docksplit")
        .Attr("dir", &DockSplitElement::SetDirectionFromString)
        .Attr("ratio", &DockSplitElement::SetRatio);

static auto s_reg_dockLeafNode =
    RegisterWithFactory<DockLeafNodeElement>("DockLeafNode", []()
                                             { return std::make_unique<DockLeafNodeElement>(); })
        .TagAlias("dockleafnode")
        .Attr("active", &DockLeafNodeElement::SetActiveTab);

static auto s_reg_dockTabNode =
    RegisterWithFactory<DockTabNodeElement>("DockTabNode", []()
                                            { return std::make_unique<DockTabNodeElement>(); })
        .TagAlias("docktabnode")
        .Attr("panel", &DockTabNodeElement::SetPanelId);

} // namespace



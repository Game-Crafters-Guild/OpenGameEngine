#include "Graph/GraphPort.h"

#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"

#include <memory>

namespace GameEngine {

GraphPort::GraphPort()
{
    AddClass("graph-port");
    SetFocusable(false);
}

void GraphPort::SetDisplayName(const std::string& name)
{
    for (const auto& child : GetChildren())
    {
        if (!child || !child->HasClass("graph-port-label"))
            continue;
        if (auto* label = dynamic_cast<Label*>(child.get()))
        {
            label->SetText(name);
            return;
        }
    }
}

void GraphPort::Reset()
{
    m_PortId.clear();
    SetDisplayName({});
    SetTooltip({});
    RemoveClass("valid-target");
    RemoveClass("has-inline-editor");
    RemoveClass("bool-editor");
    for (const auto& child : GetChildren())
    {
        if (!child)
            continue;
        if (child->HasClass("graph-inline-editors") || child->HasClass("graph-inline-field"))
            UI::Layout::SetElementHidden(*child, true);
    }
}

} // namespace GameEngine

namespace RegisterGraphElements
{
static auto s_reg_graphPort =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::GraphPort>(
        "GraphPort",
        []() { return std::make_unique<GameEngine::GraphPort>(); })
        .TagAlias("graphport");
}

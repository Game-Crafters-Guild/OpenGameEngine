#include "Inspectors/InspectorEntityActivity.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"

#include <string_view>

namespace GameEngine::Editor
{
namespace
{
// The off ancestor's own name, empty when it has none: what a rename changes.
std::string_view OffAncestorName(ECS::World& world, const EntityActivity& activity)
{
    if (!activity.OffAncestor.IsValid() || !world.IsValid(activity.OffAncestor))
        return {};
    const auto* name = world.GetComponent<Components::Name>(activity.OffAncestor);
    return name ? name->View() : std::string_view{};
}

bool IsInactiveThroughAncestor(const EntityActivity& activity)
{
    return activity.SwitchedOn && !activity.ActiveInHierarchy;
}

std::string EntityToggleTooltip(ECS::World& world, const EntityActivity& activity, bool multiEdit)
{
    std::string tooltip =
        multiEdit ? "Switch the selected entities on or off." : "Switch the selected entity on or off.";
    if (!IsInactiveThroughAncestor(activity))
        return tooltip;
    if (!activity.OffAncestor.IsValid() || !world.IsValid(activity.OffAncestor))
        return tooltip + " It is on, but inactive while a parent is off.";
    const std::string ancestor = EntityDisplayName(world, activity.OffAncestor);
    return tooltip + " It is on, but inactive while " + ancestor + " is off: switch " + ancestor +
           " on to run it.";
}
} // namespace

void InspectorEntityActivityPresenter::Bind(Toggle* toggle, Label* reasonLine, bool multiEdit)
{
    m_Toggle = toggle;
    m_ReasonLine = reasonLine;
    m_MultiEdit = multiEdit;
    m_Drawn = false;
}

EntityActivity InspectorEntityActivityPresenter::Present(ECS::World& world, ECS::EntityHandle entity)
{
    const EntityActivity activity = DescribeEntityActivity(world, entity);
    const std::string_view offAncestorName = OffAncestorName(world, activity);
    if (m_Drawn && activity == m_DrawnActivity && offAncestorName == m_DrawnOffAncestorName)
        return activity;
    m_Drawn = true;
    m_DrawnActivity = activity;
    m_DrawnOffAncestorName = offAncestorName;

    const bool inactive = IsInactiveThroughAncestor(activity);
    if (m_Toggle)
    {
        if (inactive)
            m_Toggle->AddClass("toggle-inactive");
        else
            m_Toggle->RemoveClass("toggle-inactive");
        m_Toggle->SetTooltip(EntityToggleTooltip(world, activity, m_MultiEdit));
    }
    if (m_ReasonLine)
    {
        m_ReasonLine->SetText(inactive ? EntityActivityReason(world, activity) : std::string{});
        if (inactive)
            m_ReasonLine->RemoveClass("hidden");
        else
            m_ReasonLine->AddClass("hidden");
    }
    return activity;
}

} // namespace GameEngine::Editor

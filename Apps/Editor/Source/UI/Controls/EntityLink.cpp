#include "UI/Controls/EntityLink.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine::EditorUI
{

namespace
{

constexpr const char* kStyleAssetPath = "UI/controls/EntityLink/EntityLink.css";
// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

EntityLinkActions s_EditorActions;

std::unique_ptr<Label> MakeText(std::string text, const char* cssClass)
{
    auto label = std::make_unique<Label>();
    label->SetText(std::move(text));
    label->AddClass(cssClass);
    return label;
}

// Calls `select(entity, frame)` on a left click on `element`.
void SelectOnClick(UIElement& element, const EntityLinkActions& actions, ECS::EntityHandle entity, bool frame)
{
    element.RegisterEventHandler(kEventMouseUp, [select = actions.Select, entity, frame](UIEvent& event) {
        if (event.Button != kLeftButton)
            return;
        event.Stop();
        if (select)
            select(entity, frame);
    });
}

} // namespace

std::unique_ptr<UIElement> MakeEntityLink(ECS::EntityHandle entity, const ECS::World& world,
                                          const EntityLinkActions& actions, const EntityLinkMissing& missing)
{
    if (!world.IsValid(entity))
    {
        auto gone = MakeText(missing.Text, "entity-link-missing");
        gone->SetTooltip(missing.Tooltip);
        gone->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
        return gone;
    }
    const std::string title = Editor::EntityDisplayName(world, entity);
    auto chip = std::make_unique<UIElement>();
    chip->AddClass("entity-link");
    chip->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    auto name = MakeText(title, "entity-link-name");
    name->SetTooltip("Click to select " + title);
    SelectOnClick(*name, actions, entity, false);
    chip->AddChild(std::move(name));
    auto frame = std::make_unique<UIElement>();
    frame->AddClass("entity-link-frame");
    frame->SetTooltip("Frame in Scene View");
    SelectOnClick(*frame, actions, entity, true);
    chip->AddChild(std::move(frame));
    if (actions.Hover)
    {
        chip->RegisterEventHandler(kEventMouseEnter, [hover = actions.Hover, entity](UIEvent&) { hover(entity); });
        chip->RegisterEventHandler(kEventMouseLeave, [hover = actions.Hover](UIEvent&) { hover(ECS::EntityHandle{}); });
    }
    return chip;
}

const EntityLinkActions& EditorEntityLinkActions()
{
    return s_EditorActions;
}

void SetEditorEntityLinkActions(EntityLinkActions actions)
{
    s_EditorActions = std::move(actions);
}

} // namespace GameEngine::EditorUI

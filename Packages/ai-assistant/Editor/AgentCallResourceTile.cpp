#include "AgentCallResourceTile.h"

#include "AgentCallFileMenu.h"

#include "Editor/Assets/EditorAssetActions.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine
{
namespace
{
// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

Label* AddLabel(UIElement& parent, const std::string& text, const char* className)
{
    auto label = std::make_unique<Label>();
    label->AddClass(className);
    label->SetText(text);
    Label* added = label.get();
    parent.AddChild(std::move(label));
    return added;
}
} // namespace

AgentCallResourceTile::AgentCallResourceTile(const AgentCallResource& resource, const std::string& idSuffix)
    : m_Resource(resource)
{
    SetFocusable(false);
    AddClass("agent-call-resource");
    SetId("AgentCallResource:" + idSuffix);
    // A missing asset is resolved once, when its row is built; the tile says so rather than
    // promise it will change when the file appears.
    SetTooltip(resource.Found ? resource.Path.string()
                              : "Not in the project when the call ran: " + resource.Reference);
    if (!resource.Found)
    {
        AddClass("agent-call-resource-missing");
        AddLabel(*this, resource.Name, "agent-call-resource-name");
        AddLabel(*this, "not found", "agent-call-resource-missing-text");
        return;
    }

    auto thumbnail = std::make_unique<UIElement>();
    thumbnail->AddClass("agent-call-resource-thumbnail");
    m_Thumbnail = thumbnail.get();
    AddChild(std::move(thumbnail));
    AddManipulator(ContextMenuManipulator::Create(AgentCallFileMenu(resource.Path)));
    AddLabel(*this, resource.Name, "agent-call-resource-name");
    AddLabel(*this, AssetTypeToString(resource.Type), "agent-call-resource-type");
    // A text link, like Details: it takes no plate, and Enter or Space opens.
    Label* open = AddLabel(*this, "Open", "agent-call-resource-open");
    open->SetId("AgentCallResourceOpen:" + idSuffix);
    open->SetTooltip("Open " + resource.Name + " in the editor");
    open->SetFocusable(true);
    open->RegisterEventHandler(kEventMouseUp, [this](UIEvent& event) { OnOpenMouseUp(event); });
    open->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnOpenKeyDown(event); });
}

void AgentCallResourceTile::RequestThumbnail()
{
    const auto& show = Editor::GetEditorAssetActions().ShowThumbnail;
    if (m_Thumbnail && show)
        show(*m_Thumbnail, m_Resource.Path, kThumbnailRequestPx);
}

void AgentCallResourceTile::Open()
{
    if (const auto& open = Editor::GetEditorAssetActions().Open)
        open(m_Resource.Path);
}

void AgentCallResourceTile::OnOpenMouseUp(UIEvent& event)
{
    if (event.Button != kLeftButton)
        return;
    event.Stop();
    Open();
}

void AgentCallResourceTile::OnOpenKeyDown(UIEvent& event)
{
    if (event.Key != Input::kKeyCode_Enter && event.Key != Input::kKeyCode_Space)
        return;
    event.Stop();
    Open();
}
} // namespace GameEngine

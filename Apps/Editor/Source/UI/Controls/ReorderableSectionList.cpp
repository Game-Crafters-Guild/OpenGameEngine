#include "UI/Controls/ReorderableSectionList.h"

#include "UI/Controls/Foldout.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <cmath>
#include <memory>

namespace GameEngine::EditorUI
{
namespace
{
constexpr const char* kStyleAssetPath = "UI/controls/ReorderableSectionList/ReorderableSectionList.css";
constexpr const char* kLineShownClass = "reorderable-section-list-line-shown";
// Pointer travel, in pixels, that turns a press on a header into a drag rather than a click.
constexpr float kDragThreshold = 6.0f;

// What a drag of one entry carries: the list it started in and the entry's key.
struct SectionDragPayload
{
    std::string Owner;
    uint64_t Key = 0;
};

// A press on an entry's header that may become a drag.
struct PressState
{
    float X = 0.0f;
    float Y = 0.0f;
    bool Pressed = false;
};

void BeginPress(PressState& state, const UIEvent& event)
{
    if (event.Button != 0)
        return;
    state.X = event.X;
    state.Y = event.Y;
    state.Pressed = true;
}

// Starts the drag once the pressed pointer has moved past the threshold, cancelling the press so
// the section does not also toggle.
void ContinuePress(PressState& state, Foldout* section, const std::string& owner, uint64_t key, UIEvent& event)
{
    if (!state.Pressed || std::hypot(event.X - state.X, event.Y - state.Y) < kDragThreshold)
        return;
    state.Pressed = false;
    auto* ui = section->GetOwnerManager();
    auto* manager = ui ? ui->GetDragDropManager() : nullptr;
    if (!manager)
        return;
    UIEvent cancel;
    cancel.Id = kEventMouseCancel;
    section->OnEvent(cancel);
    ui->ReleaseMouseCapture();
    auto payload = UI::Interaction::DragPayload::Create(SectionDragPayload{owner, key});
    payload.DisplayLabel = section->GetTitle();
    manager->BeginDrag(std::move(payload));
    event.Stop();
}
} // namespace

ReorderableSectionList::ReorderableSectionList(std::string owner, ReorderFn reorder)
    : m_Owner(std::move(owner)), m_Reorder(std::move(reorder))
{
    AddClass("reorderable-section-list");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    auto line = std::make_unique<UIElement>();
    line->AddClass("reorderable-section-list-line");
    m_Line = line.get();
    AddChild(std::move(line));
}

void ReorderableSectionList::AddEntry(uint64_t key, Foldout* section, const std::string& tooltip)
{
    if (!section)
        return;
    m_Entries.emplace_back(key, section);
    auto press = std::make_shared<PressState>();
    auto* header = section->GetHeader();
    header->SetTooltip(tooltip);
    header->RegisterEventHandler(kEventMouseDown, [press](UIEvent& event) { BeginPress(*press, event); });
    section->RegisterEventHandler(kEventMouseMove, [press, section, owner = m_Owner, key](UIEvent& event)
                                  { ContinuePress(*press, section, owner, key, event); });
    section->RegisterEventHandler(kEventMouseUp, [press](UIEvent&) { press->Pressed = false; });
    section->RegisterEventHandler(kEventMouseCancel, [press](UIEvent&) { press->Pressed = false; });
}

bool ReorderableSectionList::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<SectionDragPayload>();
}

bool ReorderableSectionList::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y) || m_Entries.empty())
        return false;
    for (const auto& [key, section] : m_Entries)
    {
        const float top = section->GetLayoutY();
        const float height = section->GetLayoutHeight();
        if (y < top + height)
        {
            out.TargetId = key;
            out.Location = y < top + height * 0.5f ? UI::Interaction::DropLocation::BeforeItem
                                                   : UI::Interaction::DropLocation::AfterItem;
            return true;
        }
    }
    out.TargetId = m_Entries.back().first;
    out.Location = UI::Interaction::DropLocation::AfterItem;
    return true;
}

UI::Interaction::DropFeedback ReorderableSectionList::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<SectionDragPayload>();
    return {payload && payload->Owner == m_Owner && payload->Key != request.hit.TargetId};
}

void ReorderableSectionList::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (!CanDrop(request).Allowed || !m_Reorder)
        return;
    const auto& payload = *request.payload.TryGet<SectionDragPayload>();
    m_Reorder(payload.Key, request.hit.TargetId, request.hit.Location == UI::Interaction::DropLocation::AfterItem);
}

void ReorderableSectionList::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    m_Line->RemoveClass(kLineShownClass);
    if (!state.Visible || !state.Allowed)
        return;
    for (const auto& [key, section] : m_Entries)
    {
        if (key != state.Hit.TargetId)
            continue;
        const bool after = state.Hit.Location == UI::Interaction::DropLocation::AfterItem;
        const float y = section->GetLayoutY() - GetLayoutY() + (after ? section->GetLayoutHeight() : 0.0f);
        m_Line->Overrides()
            .Set(Style::PositionTop, StyleLength::Px(y))
            .Set(Style::Width, StyleLength::Px(GetLayoutWidth()));
        m_Line->AddClass(kLineShownClass);
        return;
    }
}

} // namespace GameEngine::EditorUI

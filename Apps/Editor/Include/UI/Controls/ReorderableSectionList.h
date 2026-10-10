#pragma once

#include "UI/Interaction/DropTarget.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
class Foldout;

namespace EditorUI
{

/// A vertical list of sections the user reorders by dragging a section's header, with a line where
/// the dragged section will land. The owner adds its sections as children and registers each with
/// AddEntry under a key it chooses (a position, a stable id); a drop reports the dragged key, the
/// key it landed on and whether it landed after that entry, and the owner applies the move. Drags
/// only land in the list they started from, named by `owner`. The line's look comes from
/// UI/controls/ReorderableSectionList/ReorderableSectionList.css.
class ReorderableSectionList final : public UIElement, public UI::Interaction::IDropTarget
{
  public:
    using ReorderFn = std::function<void(uint64_t sourceKey, uint64_t targetKey, bool after)>;

    ReorderableSectionList(std::string owner, ReorderFn reorder);

    /// Makes `section`, a child of this list, a draggable entry: a left press on its header that
    /// moves past the drag threshold starts the drag. `tooltip` goes on the header.
    void AddEntry(uint64_t key, Foldout* section, const std::string& tooltip);

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

  private:
    std::string m_Owner;
    ReorderFn m_Reorder;
    std::vector<std::pair<uint64_t, Foldout*>> m_Entries;
    UIElement* m_Line = nullptr;
};

} // namespace EditorUI
} // namespace GameEngine

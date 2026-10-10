#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::MarkupECS
{
struct MarkupEntry;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The Mark-ups panel's row model: the order of its rows and the row each mark-up or thread
// entry becomes. The panel owns the frame, the filters and when the rows are rebuilt.

// Newest change first; mark-ups changed in the same second order by Revision, newest first,
// so a rebuild never swaps them.
void SortMarkupsNewestFirst(const ECS::World& world, std::vector<ECS::EntityHandle>& markups);

// One thread entry of a mark-up, as the Activity tab lists it.
struct MarkupActivityItem
{
    ECS::EntityHandle Entity{};
    const MarkupECS::MarkupEntry* Entry = nullptr;
    std::size_t Index = 0; // the entry's position in its mark-up's thread
};

// Newest entry first; entries stamped in the same second order by their position in the
// thread, later first, so a status change and the comment after it read newest on top.
void SortActivityNewestFirst(std::vector<MarkupActivityItem>& items);

// Whether `entity`'s rows show under the panel's search and status filter: its title holds
// `query` (any case; empty matches all) and its current status is `statusFilter` (a status
// tag id as text; empty matches all). Both tabs filter through it.
bool MarkupMatchesFilters(const ECS::World& world, ECS::EntityHandle entity, std::string_view query,
                          std::string_view statusFilter);

// What a row does when the user acts on it.
struct MarkupRowActions
{
    // A left-button release on the row.
    std::function<void(ECS::EntityHandle)> Clicked;
    // The row changed what the panel shows (the eye hid or showed its mark-up).
    std::function<void()> Changed;
};

// A Mark-ups tab row: the status pill, a dot in the color the mark-up draws in, the kind glyph
// (box, sphere, region), the title, a region's outline badge ("outline needs three points",
// "crosses itself"; its tooltip names the fix), who changed it last and when, the unseen dot and
// the eye, which hides or shows this row's mark-up
// (a view state, not a scene edit).
// Hovering the row outlines its mark-up in the Scene View. A `selected` row (its mark-up is in
// the editor's selection, which the Inspector shows) carries the "selected" class; the unseen
// dot is a state of its own.
std::unique_ptr<UIElement> BuildMarkupRow(MarkupEditorBridge& bridge, const ECS::World& world, ECS::EntityHandle entity,
                                          int64 now, bool selected, const MarkupRowActions& actions);

// An Activity tab row: one thread entry as the mark-up's title, the author in a fixed column,
// the kind's marker (BuildMarkupEntryMarker), what was said or done, when, and the Mark-ups
// tab's unseen dot. It carries its kind's class (MarkupEntryKindClass) and, for
// an action, markup-entry-action, styled as the inspector's Thread styles them; `alternate`
// rows take the alternating plate. Highlighted while it is an agent entry the viewer has not
// seen (MarkupEditorBridge::FirstUnseenEntry), which also carries the dot. A comment's links and
// entity links work in it (BuildMarkupCommentText).
std::unique_ptr<UIElement> BuildActivityRow(MarkupEditorBridge& bridge, ECS::World& world,
                                            const MarkupActivityItem& item, int64 now, bool alternate,
                                            const MarkupRowActions& actions);

} // namespace GameEngine::Editor

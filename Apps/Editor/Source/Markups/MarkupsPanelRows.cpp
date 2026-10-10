#include "Markups/MarkupsPanelRows.h"

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupCommentText.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupPresentation.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <optional>
#include <string>

namespace GameEngine::Editor
{

namespace
{

using Components::Markup;
using Components::MarkupAuthor;

// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

std::unique_ptr<Label> MakeLabel(const std::string& text, const char* cssClass)
{
    auto label = std::make_unique<Label>();
    label->SetText(text);
    label->AddClass(cssClass);
    return label;
}

std::unique_ptr<Label> MakeStatusPill(uint32 status)
{
    const std::optional<uint32_t> fill = MarkupTagArgb(status);
    if (!fill)
    {
        // A status the vocabulary does not hold: the theme's look for it, as the
        // inspector's "Unknown" entry.
        auto unknown = MakeLabel("Unknown", "markups-status-pill");
        unknown->AddClass("markups-status-pill-unknown");
        return unknown;
    }
    auto pill = MakeLabel(MarkupTagLabel(status), "markups-status-pill");
    if (MarkupTagReadsDarkText(status))
        pill->AddClass("markups-status-pill-dark-text");
    // The fill is the vocabulary's color for the status: project data, not a theme color.
    pill->Overrides().Set(Style::BackgroundColor, *fill);
    return pill;
}

// Hover outlines the row's mark-up; a left-button release is the row's click.
void WireRowPointer(UIElement& row, MarkupEditorBridge& bridge, ECS::EntityHandle entity,
                    const MarkupRowActions& actions)
{
    row.RegisterEventHandler(kEventMouseEnter, [&bridge, entity](UIEvent&) { bridge.HoverMarkup(entity); });
    row.RegisterEventHandler(kEventMouseLeave, [&bridge](UIEvent&) { bridge.HoverMarkup(ECS::EntityHandle{}); });
    row.RegisterEventHandler(kEventMouseUp, [clicked = actions.Clicked, entity](UIEvent& event) {
        if (event.Button == kLeftButton && clicked)
            clicked(entity);
    });
}

} // namespace

bool MarkupMatchesFilters(const ECS::World& world, ECS::EntityHandle entity, std::string_view query,
                          std::string_view statusFilter)
{
    const Markup* markup = world.GetComponent<Markup>(entity);
    if (!markup || (!statusFilter.empty() && std::to_string(markup->Status) != statusFilter))
        return false;
    if (query.empty())
        return true;
    const std::string title = MarkupTitle(world, entity);
    for (size_t i = 0; i + query.size() <= title.size(); ++i)
    {
        if (EqualsIgnoreCase(std::string_view(title).substr(i, query.size()), query))
            return true;
    }
    return false;
}

void SortMarkupsNewestFirst(const ECS::World& world, std::vector<ECS::EntityHandle>& markups)
{
    std::sort(markups.begin(), markups.end(), [&world](ECS::EntityHandle a, ECS::EntityHandle b) {
        const Markup& left = *world.GetComponent<Markup>(a);
        const Markup& right = *world.GetComponent<Markup>(b);
        if (left.UpdatedUnix != right.UpdatedUnix)
            return left.UpdatedUnix > right.UpdatedUnix;
        return left.Revision > right.Revision;
    });
}

void SortActivityNewestFirst(std::vector<MarkupActivityItem>& items)
{
    std::stable_sort(items.begin(), items.end(), [](const MarkupActivityItem& a, const MarkupActivityItem& b) {
        if (a.Entry->TimeUnix != b.Entry->TimeUnix)
            return a.Entry->TimeUnix > b.Entry->TimeUnix;
        return a.Index > b.Index;
    });
}

std::unique_ptr<UIElement> BuildMarkupRow(MarkupEditorBridge& bridge, const ECS::World& world, ECS::EntityHandle entity,
                                          int64 now, bool selected, const MarkupRowActions& actions)
{
    const Markup& markup = *world.GetComponent<Markup>(entity);
    const bool hidden = bridge.IsHidden(world, entity);

    auto row = std::make_unique<UIElement>();
    row->AddClass("markups-row");
    if (selected)
        row->AddClass("selected");
    if (hidden)
        row->AddClass("markups-row-hidden");
    row->SetTooltip("Click to select, double-click to frame");
    row->AddChild(MakeStatusPill(markup.Status));
    // The color the volume and its Scene View label draw in: its own, else its status's.
    auto swatch = std::make_unique<UIElement>();
    swatch->AddClass("markups-color-dot");
    swatch->Overrides().Set(Style::BackgroundColor, MarkupDisplayArgb(markup));
    row->AddChild(std::move(swatch));
    row->AddChild(BuildMarkupKindGlyph(ReadMarkupKind(world, entity)));
    row->AddChild(MakeLabel(MarkupTitle(world, entity), "markups-title"));
    // A region whose outline cannot enclose its area says so on its row; the inspector names the fix.
    if (const RegionOutlineProblem problem = ReadRegionOutlineProblem(world, entity);
        problem != RegionOutlineProblem::None)
    {
        auto badge = MakeLabel(RegionOutlineProblemBadge(problem), "markups-outline-badge");
        badge->SetTooltip(RegionOutlineProblemFix(problem));
        row->AddChild(std::move(badge));
    }
    row->AddChild(MakeLabel(MarkupAuthorText(markup.UpdatedBy), "markups-author"));
    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("markups-spacer");
    row->AddChild(std::move(spacer));
    row->AddChild(MakeLabel(RelativeTimeText(markup.UpdatedUnix, now), "markups-time"));
    // A seen dot keeps its slot, so the columns line up whether or not a row has one.
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("markups-update-dot");
    if (bridge.HasUnseenUpdate(world, entity))
        dot->SetTooltip("The agent changed this since you last looked");
    else
        dot->AddClass("markups-update-dot-seen");
    row->AddChild(std::move(dot));
    auto eye = std::make_unique<Button>();
    eye->AddClass("icon-button");
    eye->AddClass("markups-eye");
    if (hidden)
        eye->AddClass("markups-eye-off");
    eye->SetTooltip(hidden ? "Show in the Scene View" : "Hide in the Scene View");
    eye->RegisterEventHandler(kEventButtonClick,
                              [&bridge, &world, entity, hidden, changed = actions.Changed](UIEvent& event) {
                                  bridge.SetHidden(world, {&entity, 1}, !hidden);
                                  event.Stop();
                                  if (changed)
                                      changed();
                              });
    row->AddChild(std::move(eye));
    WireRowPointer(*row, bridge, entity, actions);
    return row;
}

std::unique_ptr<UIElement> BuildActivityRow(MarkupEditorBridge& bridge, ECS::World& world,
                                            const MarkupActivityItem& item, int64 now, bool alternate,
                                            const MarkupRowActions& actions)
{
    const MarkupECS::MarkupEntry& entry = *item.Entry;
    auto row = std::make_unique<UIElement>();
    row->AddClass("markups-row");
    row->AddClass("markups-activity-row");
    row->AddClass(MarkupEntryKindClass(entry));
    if (MarkupEntryIsAction(entry))
        row->AddClass("markup-entry-action");
    if (alternate)
        row->AddClass("markups-activity-row-alt");
    const bool unread =
        entry.Author == MarkupAuthor::Agent && item.Index >= bridge.FirstUnseenEntry(world, item.Entity);
    if (unread)
        row->AddClass("markups-unread");
    row->SetTooltip("Click to select and frame");
    row->AddChild(MakeLabel(MarkupTitle(world, item.Entity), "markups-title"));
    row->AddChild(MakeLabel(MarkupAuthorText(entry.Author), "markups-activity-author"));
    row->AddChild(BuildMarkupEntryMarker(entry));
    if (std::unique_ptr<UIElement> swatch = BuildMarkupEntryColorSwatch(entry))
    {
        // The color the line names, right after its words.
        auto line = std::make_unique<UIElement>();
        line->AddClass("markups-entry-text");
        line->AddClass("markup-entry-line");
        auto words = MakeLabel(MarkupEntryText(entry), "markups-entry-text");
        words->AddClass("markup-entry-line-text");
        line->AddChild(std::move(words));
        line->AddChild(std::move(swatch));
        row->AddChild(std::move(line));
    }
    else
    {
        row->AddChild(entry.Kind == MarkupECS::MarkupEntryKind::Comment
                          ? BuildMarkupCommentText(entry.Text, "markups-entry-text", bridge, world,
                                                   SelectThroughBridge(bridge, world))
                          : std::unique_ptr<UIElement>(MakeLabel(MarkupEntryText(entry), "markups-entry-text")));
    }
    row->AddChild(MakeLabel(RelativeTimeText(entry.TimeUnix, now), "markups-time"));
    // The unseen dot the Mark-ups tab shows, on the agent entries the viewer has not seen.
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("markups-update-dot");
    if (!unread)
        dot->AddClass("markups-update-dot-seen");
    row->AddChild(std::move(dot));
    WireRowPointer(*row, bridge, item.Entity, actions);
    return row;
}

} // namespace GameEngine::Editor

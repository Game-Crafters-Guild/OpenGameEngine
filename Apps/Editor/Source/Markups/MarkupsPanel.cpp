#include "Markups/MarkupsPanel.h"

#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupHighlightState.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupsPanelRows.h"
#include "Platform/SystemMetrics.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/PanelSearchBar.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using MarkupECS::MarkupService;

// How often the rows are rebuilt while the panel shows, so "5 min ago" stays true.
constexpr std::chrono::minutes kTimesRefreshPeriod{1};

constexpr const char* kNoMarkupsText =
    "No mark-ups in this scene. Use the Mark-up tool in the Scene View, or ask the agent.";
constexpr const char* kNoMatchText = "No mark-ups match the search or the status filter.";

std::vector<Dropdown::Option> StatusFilterOptions()
{
    std::vector<Dropdown::Option> options;
    options.push_back(Dropdown::Option{"", "All statuses"});
    if (const MarkupService* service = MarkupService::TryGet())
    {
        for (uint32 id = 0; id < service->GetTagCount(); ++id)
        {
            if (service->IsStatusTag(id))
                options.push_back(Dropdown::Option{std::to_string(id), MarkupTagLabel(id)});
        }
    }
    return options;
}

MarkupsPanel* s_Open = nullptr;

} // namespace

MarkupsPanel::MarkupsPanel(MarkupEditorBridge& bridge)
    : AssetBoundDockPanel("Mark-ups", std::string(kAssetSourceAliasEditor), "UI/panels/MarkupsPanel.uxml", {}),
      m_Bridge(bridge)
{
    AddClass("markups-panel");

    EditorChangeNotifications& notifications = m_Bridge.GetNotifications();
    m_ComponentToken = notifications.SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& event) {
            if (event.kind != EditorChangeNotifications::ChangeKind::Preview)
                RequestRebuild();
        });
    m_StructureToken = notifications.SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent&) { RequestRebuild(); });
    s_Open = this;
}

MarkupsPanel* MarkupsPanel::TryGetOpen()
{
    return s_Open;
}

MarkupsPanel::~MarkupsPanel()
{
    if (s_Open == this)
        s_Open = nullptr;
    EditorChangeNotifications& notifications = m_Bridge.GetNotifications();
    notifications.Unsubscribe(m_ComponentToken);
    notifications.Unsubscribe(m_StructureToken);
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
}

void MarkupsPanel::OnLayoutBound()
{
    BindFrame();
    if (UIManager* manager = GetOwnerManager())
    {
        m_RefreshManager = UIManagerRef(manager);
        m_RefreshToken = manager->RegisterPeriodicRefresh([this]() { RefreshTimesIfShown(); });
    }
    ShowTab(m_Tab);
}

void MarkupsPanel::OnLayoutReconciled()
{
    BindFrame();
    ShowTab(m_Tab);
}

void MarkupsPanel::BindFrame()
{
    if (auto* tab = dynamic_cast<Button*>(FindById("MarkupsTabMarkups")); tab && tab != m_MarkupsTabButton.Get())
    {
        tab->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowTab(Tab::Markups); });
        m_MarkupsTabButton = MakeWeakRef(tab);
    }
    if (auto* tab = dynamic_cast<Button*>(FindById("MarkupsTabActivity")); tab && tab != m_ActivityTabButton.Get())
    {
        tab->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowTab(Tab::Activity); });
        m_ActivityTabButton = MakeWeakRef(tab);
    }
    if (auto* showAll = dynamic_cast<Button*>(FindById("MarkupsShowAll"));
        showAll && showAll != m_ShowAllButton.Get())
    {
        showAll->SetTooltip("Show every mark-up in the Scene View (a view setting, not a scene edit)");
        showAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetAllHidden(false); });
        m_ShowAllButton = MakeWeakRef(showAll);
    }
    if (auto* hideAll = dynamic_cast<Button*>(FindById("MarkupsHideAll"));
        hideAll && hideAll != m_HideAllButton.Get())
    {
        hideAll->SetTooltip("Hide every mark-up in the Scene View (a view setting, not a scene edit)");
        hideAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetAllHidden(true); });
        m_HideAllButton = MakeWeakRef(hideAll);
    }
    // The search bar is the shared editor control, built into the frame's host.
    if (UIElement* host = FindById("MarkupsSearchHost"); host && host != m_SearchHost.Get())
    {
        PanelSearchBarBuilt search =
            BuildPanelSearchBar("markups-search-field", {}, [this](const std::string&) { RequestRebuild(); });
        search.RootPtr->SetPlaceholder("Search mark-ups");
        search.RootPtr->AddClass("markups-search");
        m_Search = MakeWeakRef(search.FieldPtr);
        host->AddChild(std::move(search.Root));
        m_SearchHost = MakeWeakRef(host);
    }
    // The status filter stands beside the field, labelled with what it lists.
    if (auto* filter = dynamic_cast<Dropdown*>(FindById("MarkupsStatusFilter"));
        filter && filter != m_StatusFilterDropdown.Get())
    {
        filter->SetOptions(StatusFilterOptions(), 0);
        filter->SetTooltip("List the mark-ups with one status");
        filter->SetOnValueChanged([this](const std::string& status) {
            m_StatusFilter = status;
            RequestRebuild();
        });
        m_StatusFilterDropdown = MakeWeakRef(filter);
    }
    // The rows are generated into a list the scroll view holds.
    if (auto* scroll = dynamic_cast<ScrollView*>(FindById("MarkupsScroll")); scroll && scroll != m_Scroll.Get())
    {
        auto list = std::make_unique<UIElement>();
        list->AddClass("markups-list");
        m_List = MakeWeakRef(list.get());
        scroll->AddContent(std::move(list));
        m_Scroll = MakeWeakRef(scroll);
    }
    m_Empty = MakeWeakRef(dynamic_cast<Label*>(FindById("MarkupsEmpty")));
}

void MarkupsPanel::RefreshTimesIfShown()
{
    if (!IsLayoutBound() || GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return; // an inactive dock tab
    // A selection made in the Scene View or the Hierarchy reaches the rows' selected state
    // here; the panel's own clicks rebuild at once.
    if (std::chrono::steady_clock::now() - m_LastRebuildTime >= kTimesRefreshPeriod || SelectionChangedSinceRebuild())
        Rebuild();
}

void MarkupsPanel::ShowTab(Tab tab)
{
    m_Tab = tab;
    if (Button* button = m_MarkupsTabButton.Get())
        tab == Tab::Markups ? button->AddClass("active") : button->RemoveClass("active");
    if (Button* button = m_ActivityTabButton.Get())
        tab == Tab::Activity ? button->AddClass("active") : button->RemoveClass("active");
    Rebuild();
}

void MarkupsPanel::SetAllHidden(bool hidden)
{
    if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld(); world && MarkupService::TryGet())
        m_Bridge.SetHidden(*world, MarkupService::Get().GetMarkups(*world), hidden);
    RequestRebuild();
}

void MarkupsPanel::RequestRebuild()
{
    if (m_RebuildPending)
        return;
    m_RebuildPending = true;
    // A notification arrives mid-dispatch: rebuild at the next safe point, and not at
    // all once the panel is gone. Not PostSafeAction: it skips an element its manager
    // cannot reach, which a panel on an inactive dock tab is, and the skipped action
    // would hold the latch for good.
    if (!PostAction([panel = MakeWeakRef(this)]() {
            if (MarkupsPanel* self = panel.Get())
                self->Rebuild();
        }))
        m_RebuildPending = false;
}

void MarkupsPanel::Rebuild()
{
    m_RebuildPending = false;
    // The rows go, and a removed row gets no leave event: drop its hover, or its volume stays
    // highlighted under a pointer that is over no row. The row under the pointer, if any,
    // enters again after the rebuild. Only these rows set the bridge's hovered row; with none
    // set there is nothing to drop, and the Scene View's own hover is left as it is.
    if (m_Bridge.IsPanelRowHovered())
        m_Bridge.HoverMarkup({});
    UIElement* list = m_List.Get();
    if (!list)
        return;
    m_LastRebuildTime = std::chrono::steady_clock::now();
    const std::span<const ECS::EntityHandle> selected = m_Bridge.GetHighlight().Selected;
    m_ShownSelection.assign(selected.begin(), selected.end());
    list->RemoveAllChildren();
    if (m_Tab == Tab::Markups)
        BuildMarkupRows(*list);
    else
        BuildActivityRows(*list);
    if (Label* empty = m_Empty.Get())
    {
        if (!list->GetChildren().empty())
        {
            empty->AddClass("hidden");
            return;
        }
        const ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
        const bool anyMarkup = world && MarkupService::TryGet() && !MarkupService::Get().GetMarkups(*world).empty();
        empty->SetText(anyMarkup ? kNoMatchText : kNoMarkupsText);
        empty->RemoveClass("hidden");
    }
}

void MarkupsPanel::BuildMarkupRows(UIElement& list)
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world || !MarkupService::TryGet())
        return;
    std::vector<ECS::EntityHandle> markups = MarkupService::Get().GetMarkups(*world);
    SortMarkupsNewestFirst(*world, markups);
    const TextField* search = m_Search.Get();
    const std::string query = search ? search->GetValue() : std::string();
    const int64 now = m_Bridge.Now();
    const MarkupRowActions actions{[this](ECS::EntityHandle entity) { OnRowClicked(entity); },
                                   [this]() { RequestRebuild(); }};
    const MarkupHighlightState highlight = m_Bridge.GetHighlight();

    for (const ECS::EntityHandle entity : markups)
    {
        if (MarkupMatchesFilters(*world, entity, query, m_StatusFilter))
            list.AddChild(BuildMarkupRow(m_Bridge, *world, entity, now, highlight.IsSelected(entity), actions));
    }
}

bool MarkupsPanel::SelectionChangedSinceRebuild() const
{
    const std::span<const ECS::EntityHandle> selected = m_Bridge.GetHighlight().Selected;
    return !std::equal(selected.begin(), selected.end(), m_ShownSelection.begin(), m_ShownSelection.end());
}

void MarkupsPanel::BuildActivityRows(UIElement& list)
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world || !MarkupService::TryGet())
        return;
    // Mark-ups newest first, so entries of different mark-ups stamped in one second keep
    // the later mark-up's first through the stable sort.
    std::vector<ECS::EntityHandle> markups = MarkupService::Get().GetMarkups(*world);
    SortMarkupsNewestFirst(*world, markups);
    std::vector<MarkupActivityItem> items;
    for (const ECS::EntityHandle entity : markups)
    {
        const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(*world, entity);
        if (!notes)
            continue;
        for (std::size_t index = 0; index < notes->Entries.size(); ++index)
        {
            if (notes->Entries[index].Kind != MarkupECS::MarkupEntryKind::Unreadable)
                items.push_back(MarkupActivityItem{entity, &notes->Entries[index], index});
        }
    }
    SortActivityNewestFirst(items);
    const TextField* search = m_Search.Get();
    const std::string query = search ? search->GetValue() : std::string();
    const int64 now = m_Bridge.Now();
    const MarkupRowActions actions{[this](ECS::EntityHandle entity) { OnActivityRowClicked(entity); },
                                   [this]() { RequestRebuild(); }};

    bool alternate = false;
    for (const MarkupActivityItem& item : items)
    {
        if (!MarkupMatchesFilters(*world, item.Entity, query, m_StatusFilter))
            continue;
        list.AddChild(BuildActivityRow(m_Bridge, *world, item, now, alternate, actions));
        alternate = !alternate;
    }
}

void MarkupsPanel::OnRowClicked(ECS::EntityHandle entity)
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return;
    const auto now = std::chrono::steady_clock::now();
    const bool doubleClick = entity == m_LastClicked && now - m_LastClickTime <= Platform::GetDoubleClickInterval();
    m_LastClicked = entity;
    m_LastClickTime = now;
    m_Bridge.SelectMarkup(*world, entity, doubleClick);
    RequestRebuild();
}

void MarkupsPanel::OnActivityRowClicked(ECS::EntityHandle entity)
{
    if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
        m_Bridge.SelectMarkup(*world, entity, true);
    RequestRebuild();
}

} // namespace GameEngine::Editor

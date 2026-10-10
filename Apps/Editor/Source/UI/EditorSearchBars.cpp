#include "UI/EditorSearchBars.h"

#include <algorithm>

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine {

bool EditorSearchBars::GetVisible()
{
    // Load on first access if not yet loaded
    if (!s_Loaded)
        LoadFromPreferences();
    return s_Visible;
}

void EditorSearchBars::SetVisible(bool visible)
{
    s_Visible = visible;
    s_Loaded = true;
    SaveToPreferences();
    PruneNulls();

    for (const RegisteredBar& registered : s_Bars)
    {
        if (visible)
            registered.SearchBar->RemoveClass("hidden");
        else
            registered.SearchBar->AddClass("hidden");
    }
}

bool EditorSearchBars::GetAtTop()
{
    if (!s_Loaded)
        LoadFromPreferences();
    return s_AtTop;
}

bool EditorSearchBars::GetAccentFocusOutline()
{
    if (!s_Loaded)
        LoadFromPreferences();
    return s_AccentFocusOutline;
}

void EditorSearchBars::SetAccentFocusOutline(bool enabled)
{
    s_AccentFocusOutline = enabled;
    s_Loaded = true;
    SaveToPreferences();

    for (UIElement* target : s_FocusTargets)
    {
        ApplyFocusOutline(target);
        if (target)
            target->MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
    }
}

void EditorSearchBars::SetAtTop(bool atTop)
{
    s_AtTop = atTop;
    s_Loaded = true;
    SaveToPreferences();
    PruneNulls();

    std::vector<UIManager*> managersToRefresh;
    for (const RegisteredBar& registered : s_Bars)
    {
        UIElement* bar = registered.PlacementTarget;
        ApplyPlacement(bar);

        UIElement* dirtyRoot = bar->GetParent() ? bar->GetParent() : bar;
        dirtyRoot->MarkDirtySubtree(
            UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);

        UIManager* manager = bar->GetOwnerManager();
        if (manager && std::find(managersToRefresh.begin(), managersToRefresh.end(), manager) ==
                           managersToRefresh.end())
            managersToRefresh.push_back(manager);
    }

    for (UIManager* manager : managersToRefresh)
        manager->RequestRelayout();
}

void EditorSearchBars::Register(UIElement* searchBar)
{
    RegisterHosted(searchBar, searchBar);
}

void EditorSearchBars::RegisterHosted(UIElement* searchBar, UIElement* placementHost)
{
    if (!searchBar || !placementHost)
        return;

    // Load on first register if not yet loaded
    if (!s_Loaded)
        LoadFromPreferences();

    // Ensure the bar matches current global state immediately.
    if (!s_Visible)
        searchBar->AddClass("hidden");
    else
        searchBar->RemoveClass("hidden");
    ApplyPlacement(placementHost);
    RegisterFocusTarget(searchBar);

    auto it = std::find_if(s_Bars.begin(), s_Bars.end(),
                           [searchBar](const RegisteredBar& registered)
                           { return registered.SearchBar == searchBar; });
    if (it == s_Bars.end())
        s_Bars.push_back({searchBar, placementHost});
    else
        it->PlacementTarget = placementHost;
}

void EditorSearchBars::RegisterFocusTarget(UIElement* searchBar)
{
    if (!searchBar)
        return;
    if (!s_Loaded)
        LoadFromPreferences();
    ApplyFocusOutline(searchBar);
    if (std::find(s_FocusTargets.begin(), s_FocusTargets.end(), searchBar) == s_FocusTargets.end())
        s_FocusTargets.push_back(searchBar);
}

void EditorSearchBars::UnregisterFocusTarget(UIElement* searchBar)
{
    auto it = std::remove(s_FocusTargets.begin(), s_FocusTargets.end(), searchBar);
    if (it != s_FocusTargets.end())
        s_FocusTargets.erase(it, s_FocusTargets.end());
}

void EditorSearchBars::ApplyFocusOutline(UIElement* searchBar)
{
    if (!searchBar)
        return;
    if (s_AccentFocusOutline)
        searchBar->AddClass("search-accent-focus");
    else
        searchBar->RemoveClass("search-accent-focus");
}

void EditorSearchBars::ApplyPlacement(UIElement* placementTarget)
{
    if (!placementTarget)
        return;

    if (s_AtTop)
        placementTarget->AddClass("search-bar-at-top");
    else
        placementTarget->RemoveClass("search-bar-at-top");

    UIElement* parent = placementTarget->GetParent();
    if (!parent)
        return;

    if (s_AtTop)
        parent->AddClass("search-bars-at-top");
    else
        parent->RemoveClass("search-bars-at-top");
}

void EditorSearchBars::Unregister(UIElement* searchBar)
{
    if (!searchBar)
        return;

    auto it = std::remove_if(s_Bars.begin(), s_Bars.end(),
                             [searchBar](const RegisteredBar& registered)
                             { return registered.SearchBar == searchBar; });
    if (it != s_Bars.end())
        s_Bars.erase(it, s_Bars.end());
    UnregisterFocusTarget(searchBar);
}

void EditorSearchBars::PruneNulls()
{
    auto it = std::remove_if(s_Bars.begin(), s_Bars.end(),
                             [](const RegisteredBar& registered)
                             { return !registered.SearchBar || !registered.PlacementTarget; });
    if (it != s_Bars.end())
        s_Bars.erase(it, s_Bars.end());
}

void EditorSearchBars::LoadFromPreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    
    prefs.TryGetBool("ui.searchBarsVisible", s_Visible);
    prefs.TryGetBool("ui.searchBarsAtTop", s_AtTop);
    prefs.TryGetBool("ui.searchBarsAccentFocusOutline", s_AccentFocusOutline);
    s_Loaded = true;
}

void EditorSearchBars::SaveToPreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err); // Load existing to preserve other settings
    
    prefs.SetBool("ui.searchBarsVisible", s_Visible);
    prefs.SetBool("ui.searchBarsAtTop", s_AtTop);
    prefs.SetBool("ui.searchBarsAccentFocusOutline", s_AccentFocusOutline);
    prefs.Save(&err);
}

} // namespace GameEngine

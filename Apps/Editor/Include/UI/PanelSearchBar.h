#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "UI/UIEvents.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"

namespace GameEngine {

struct PanelSearchBarBuilt {
    std::unique_ptr<UIElement> Root;
    SearchFieldWithFilter* RootPtr = nullptr;
    UIElement* IconPtr = nullptr;
    Button* ClearButtonPtr = nullptr;
    TextField* FieldPtr = nullptr;
    Dropdown* FilterPtr = nullptr;
};

// Builds the standard editor panel search bar:
// - `.panel-search-bar` root with search field, icon, clear action, and an optional filter dropdown
// - Handles focus/active styling and the "auto-hide when empty & globally hidden" behavior
//
// `fieldId` names the TextField for automation (debug-server input_text /
// click_element target by element id) — unique per window, e.g.
// "hierarchy-search-field".
//
// The caller is responsible for:
// - Adding the returned `root` into the UI tree
// - Registering/unregistering the returned `rootPtr` with SettingsPanel::RegisterSearchBar/UnregisterSearchBar
inline PanelSearchBarBuilt BuildPanelSearchBar(
    const std::string& fieldId,
    std::function<bool()> getGlobalVisible,
    std::function<void(const std::string&)> onTextChanged = {},
    std::function<void(const std::string&)> onTextChanging = {},
    const std::vector<Dropdown::Option>& filterOptions = {},
    std::function<void(const std::string&)> onFilterChanged = {})
{
    PanelSearchBarBuilt out{};

    auto searchBar = std::make_unique<SearchFieldWithFilter>();
    searchBar->AddClass("panel-search-bar");
    if (getGlobalVisible && !getGlobalVisible())
        searchBar->AddClass("hidden");

    SearchFieldWithFilter* searchBarPtr = searchBar.get();
    searchBar->SetFieldId(fieldId);
    searchBar->SetValue("");
    searchBar->SetFilterOptions(filterOptions, 0);
    searchBar->SetOnFilterChanged(std::move(onFilterChanged));

    TextField* fieldPtr = searchBar->GetField();
    UIElement* iconPtr = searchBar->GetSearchIcon();
    Dropdown* filterPtr = filterOptions.empty() ? nullptr : searchBar->GetFilter();
    fieldPtr->AddClass("panel-search-field");
    iconPtr->AddClass("panel-search-icon");
    searchBar->GetFilter()->AddClass("panel-search-filter");
    searchBar->GetClearButton()->AddClass("panel-search-clear");

    searchBar->SetOnQueryChanging(
        [searchBarPtr, getGlobalVisible, onTextChanged, onTextChanging](const std::string& value) {
            if (!value.empty() && searchBarPtr->HasClass("hidden"))
                searchBarPtr->RemoveClass("hidden");
            if (value.empty() && getGlobalVisible && !getGlobalVisible())
                searchBarPtr->AddClass("hidden");
            if (onTextChanging)
                onTextChanging(value);
            else if (onTextChanged)
                onTextChanged(value);
        });
    searchBar->SetOnQueryChanged(std::move(onTextChanged));

    // Focus styling (keep consistent with typing behavior)
    fieldPtr->RegisterEventHandler(kEventFocusIn, [searchBarPtr, iconPtr](UIEvent&) {
        if (searchBarPtr->HasClass("hidden"))
            searchBarPtr->RemoveClass("hidden");
        if (!searchBarPtr->HasClass("active"))
            searchBarPtr->AddClass("active");
        if (iconPtr && !iconPtr->HasClass("icon-active"))
            iconPtr->AddClass("icon-active");
    });

    fieldPtr->RegisterEventHandler(kEventFocusOut, [searchBarPtr, iconPtr, getGlobalVisible, fieldPtr](UIEvent&) {
        if (fieldPtr && fieldPtr->GetValue().empty())
        {
            if (getGlobalVisible && !getGlobalVisible() && !searchBarPtr->HasClass("hidden"))
                searchBarPtr->AddClass("hidden");
            searchBarPtr->RemoveClass("active");
            if (iconPtr)
                iconPtr->RemoveClass("icon-active");
        }
    });

    out.RootPtr = searchBarPtr;
    out.IconPtr = iconPtr;
    out.ClearButtonPtr = searchBarPtr->GetClearButton();
    out.FieldPtr = fieldPtr;
    out.FilterPtr = filterPtr;
    out.Root = std::move(searchBar);
    return out;
}

} // namespace GameEngine

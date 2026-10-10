#pragma once

#include "UI/UIElement.h"
#include "UI/Controls/Dropdown.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

class Button;
class Label;
class TextField;

/// Shared search-field chrome used by panel searches and SearchDialog.
/// Owns the query field, placeholder, search icon, optional filter dropdown,
/// and clear action as siblings so overlays and pointer routing are consistent.
class SearchFieldWithFilter : public UIElement
{
public:
    using QueryCallback = std::function<void(const std::string&)>;

    SearchFieldWithFilter();

    TextField* GetField() const { return m_Field; }
    UIElement* GetSearchIcon() const { return m_SearchIcon; }
    Dropdown* GetFilter() const { return m_Filter; }
    Button* GetClearButton() const { return m_ClearButton; }

    void SetFieldId(const std::string& id);
    void SetPlaceholder(const std::string& text);
    void SetFilterOptions(const std::vector<Dropdown::Option>& options, int selectedIndex = 0);

    void SetOnQueryChanging(QueryCallback callback) { m_OnQueryChanging = std::move(callback); }
    void SetOnQueryChanged(QueryCallback callback) { m_OnQueryChanged = std::move(callback); }
    void SetOnFilterChanged(QueryCallback callback) { m_OnFilterChanged = std::move(callback); }
    void SetRefocusAfterClear(bool enabled) { m_RefocusAfterClear = enabled; }

    const std::string& GetValue() const;
    void SetValue(const std::string& value);
    void RefreshVisualState();

private:
    void ApplyQueryVisualState(const std::string& value);

    TextField* m_Field = nullptr;
    Label* m_Placeholder = nullptr;
    UIElement* m_SearchIcon = nullptr;
    Dropdown* m_Filter = nullptr;
    Button* m_ClearButton = nullptr;
    bool m_RefocusAfterClear = false;
    std::string m_DefaultFilterValue;
    QueryCallback m_OnQueryChanging;
    QueryCallback m_OnQueryChanged;
    QueryCallback m_OnFilterChanged;
};

} // namespace GameEngine

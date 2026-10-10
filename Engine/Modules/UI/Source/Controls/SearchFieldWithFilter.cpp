#include "UI/Controls/SearchFieldWithFilter.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIManager.h"

#include <memory>

namespace GameEngine
{

SearchFieldWithFilter::SearchFieldWithFilter()
{
    AddClass("search-field-with-filter");

    auto field = std::make_unique<TextField>();
    m_Field = field.get();
    m_Field->AddClass("search-field-with-filter-input");
    m_Field->SetFocusable(true);
    m_Field->SetOnValueChanging([this](const std::string& value) {
        if (UIManager* owner = GetOwnerManager())
            owner->DismissActiveTooltip();
        ApplyQueryVisualState(value);
        if (m_OnQueryChanging)
            m_OnQueryChanging(value);
    });
    m_Field->SetOnValueChanged([this](const std::string& value) {
        if (m_OnQueryChanged)
            m_OnQueryChanged(value);
    });
    m_Field->RegisterEventHandler(kEventFocusIn, [this](UIEvent&) {
        AddClass("active");
        if (m_SearchIcon)
            m_SearchIcon->AddClass("icon-active");
    });
    m_Field->RegisterEventHandler(kEventFocusOut, [this](UIEvent&) {
        if (m_Field && m_Field->GetValue().empty())
        {
            RemoveClass("active");
            if (m_SearchIcon)
                m_SearchIcon->RemoveClass("icon-active");
        }
    });
    AddChild(std::move(field));

    auto placeholder = std::make_unique<Label>();
    m_Placeholder = placeholder.get();
    m_Placeholder->AddClass("search-field-with-filter-placeholder");
    AddChild(std::move(placeholder));

    auto icon = std::make_unique<UIElement>();
    m_SearchIcon = icon.get();
    m_SearchIcon->AddClass("search-field-with-filter-icon");
    AddChild(std::move(icon));

    auto filter = std::make_unique<Dropdown>();
    m_Filter = filter.get();
    m_Filter->AddClass("search-field-with-filter-dropdown");
    m_Filter->AddClass("hidden");
    m_Filter->SetFocusProxy(m_Field);
    m_Filter->SetAutoWidthPopup(true);
    m_Filter->SetOnValueChanged([this](const std::string& value) {
        if (value == m_DefaultFilterValue)
            RemoveClass("search-filter-active");
        else
            AddClass("search-filter-active");
        if (m_OnFilterChanged)
            m_OnFilterChanged(value);
    });
    AddChild(std::move(filter));

    auto clearButton = std::make_unique<Button>();
    m_ClearButton = clearButton.get();
    m_ClearButton->AddClass("icon-button");
    m_ClearButton->AddClass("search-field-with-filter-clear");
    m_ClearButton->AddClass("xclose-icon");
    m_ClearButton->AddClass("hidden");
    m_ClearButton->SetFocusable(false);
    m_ClearButton->SetTooltip("Clear search");
    m_ClearButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!m_Field || m_Field->GetValue().empty())
            return;
        // Resolve focus before callbacks: they may close the owning popup or
        // intentionally transfer focus. No later action may undo that choice.
        if (m_RefocusAfterClear)
            if (UIManager* owner = GetOwnerManager())
                owner->SetFocusById(m_Field->GetId());
        m_Field->SetValue("");
        ApplyQueryVisualState("");
        if (m_OnQueryChanging)
            m_OnQueryChanging("");
        else if (m_OnQueryChanged)
            m_OnQueryChanged("");
    });
    AddChild(std::move(clearButton));
}

void SearchFieldWithFilter::SetFieldId(const std::string& id)
{
    if (!m_Field)
        return;
    m_Field->SetId(id);
    if (m_Filter)
        m_Filter->SetId(id.empty() ? std::string{} : id + "-filter");
}

void SearchFieldWithFilter::SetPlaceholder(const std::string& text)
{
    if (m_Placeholder)
        m_Placeholder->SetText(text);
}

void SearchFieldWithFilter::SetFilterOptions(const std::vector<Dropdown::Option>& options, int selectedIndex)
{
    if (!m_Filter)
        return;
    if (options.empty())
    {
        m_DefaultFilterValue.clear();
        m_Filter->AddClass("hidden");
        RemoveClass("has-search-filter");
        RemoveClass("search-filter-active");
        return;
    }

    m_DefaultFilterValue = options.front().value;
    m_Filter->SetOptions(options, selectedIndex);
    m_Filter->RemoveClass("hidden");
    AddClass("has-search-filter");
    if (m_Filter->GetSelectedValue() == m_DefaultFilterValue)
        RemoveClass("search-filter-active");
    else
        AddClass("search-filter-active");
}

const std::string& SearchFieldWithFilter::GetValue() const
{
    static const std::string empty;
    return m_Field ? m_Field->GetValue() : empty;
}

void SearchFieldWithFilter::SetValue(const std::string& value)
{
    if (m_Field)
        m_Field->SetValue(value);
    ApplyQueryVisualState(value);
}

void SearchFieldWithFilter::RefreshVisualState()
{
    ApplyQueryVisualState(GetValue());
}

void SearchFieldWithFilter::ApplyQueryVisualState(const std::string& value)
{
    const bool hasQuery = !value.empty();
    if (hasQuery)
    {
        AddClass("active");
        AddClass("has-query");
    }
    else
    {
        RemoveClass("active");
        RemoveClass("has-query");
    }

    if (m_SearchIcon)
    {
        if (hasQuery)
            m_SearchIcon->AddClass("icon-active");
        else
            m_SearchIcon->RemoveClass("icon-active");
    }
    if (m_ClearButton)
    {
        if (hasQuery)
            m_ClearButton->RemoveClass("hidden");
        else
            m_ClearButton->AddClass("hidden");
    }
    if (m_Placeholder)
    {
        if (hasQuery)
            m_Placeholder->AddClass("hidden");
        else
            m_Placeholder->RemoveClass("hidden");
    }
}

} // namespace GameEngine

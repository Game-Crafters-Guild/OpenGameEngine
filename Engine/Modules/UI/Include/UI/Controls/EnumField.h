#pragma once

#include "UI/Controls/Dropdown.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

// Metadata entry mapping an enum value to a display label.
template <typename T>
struct EnumEntry
{
    T value;
    const char* label;
    // Optional CSS class carrying this entry's icon; the stylesheet owns which
    // image that is. Null or empty leaves the entry text-only.
    const char* iconClass = nullptr;
};

// Type-safe enum dropdown control. Wraps a Dropdown with automatic
// enum-to-string and string-to-enum mapping so callers never deal
// with raw indices or string conversions.
//
// Usage:
//   static constexpr EnumEntry<LightType> kLightTypes[] = {
//       {LightType::Directional, "Directional"},
//       {LightType::Point,       "Point"},
//       {LightType::Spot,        "Spot"},
//       {LightType::Ambient,     "Ambient"},
//   };
//   auto field = std::make_unique<EnumField<LightType>>();
//   field->SetEntries(kLightTypes, currentValue);
//   field->SetOnValueChanged([](LightType v) { ... });
//
template <typename T>
class EnumField : public UIElement
{
public:
    EnumField()
    {
        auto dd = std::make_unique<Dropdown>();
        m_Dropdown = dd.get();
        AddChild(std::move(dd));
    }

    // Populate from a C array of EnumEntry. Selects the entry matching
    // currentValue, or index 0 if no match is found.
    template <size_t N>
    void SetEntries(const EnumEntry<T> (&entries)[N], T currentValue)
    {
        m_Entries.assign(entries, entries + N);
        ApplyEntries(currentValue);
    }

    // Populate from a vector of EnumEntry.
    void SetEntries(std::vector<EnumEntry<T>> entries, T currentValue)
    {
        m_Entries = std::move(entries);
        ApplyEntries(currentValue);
    }

    void SetOnValueChanged(std::function<void(T)> cb)
    {
        m_Callback = std::move(cb);

        m_Dropdown->SetOnValueChanged([this](const std::string& value)
        {
            if (!m_Callback)
                return;
            int idx = 0;
            try { idx = std::stoi(value); }
            catch (...) { return; }
            if (idx >= 0 && idx < static_cast<int>(m_Entries.size()))
                m_Callback(m_Entries[static_cast<size_t>(idx)].value);
        });
    }

    Dropdown* GetDropdown() const { return m_Dropdown; }

private:
    void ApplyEntries(T currentValue)
    {
        std::vector<Dropdown::Option> opts;
        opts.reserve(m_Entries.size());
        int selectedIndex = 0;
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            opts.push_back({std::to_string(i), m_Entries[i].label, {},
                            m_Entries[i].iconClass ? m_Entries[i].iconClass : ""});
            if (m_Entries[i].value == currentValue)
                selectedIndex = static_cast<int>(i);
        }
        m_Dropdown->SetOptions(opts, selectedIndex);
    }

    Dropdown* m_Dropdown = nullptr;
    std::vector<EnumEntry<T>> m_Entries;
    std::function<void(T)> m_Callback;
};

} // namespace GameEngine

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "UI/AssetField.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/EntityField.h"
#include "UI/InfoCard.h"
#include "UI/UIElement.h"

namespace GameEngine::InspectorUI
{
// Row builders: a label cell and a field cell per row, the layout every inspector row shares.

inline UIElement* AddRow(UIElement* parent)
{
    if (!parent)
        return nullptr;
    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");
    UIElement* raw = row.get();
    parent->AddChild(std::move(row));
    return raw;
}

inline Label* AddLabel(UIElement* row, const std::string& text, const char* tooltip = nullptr)
{
    if (!row)
        return nullptr;
    auto cell = std::make_unique<UIElement>();
    cell->AddClass("inspector-label-cell");
    auto label = std::make_unique<Label>();
    label->AddClass("inspector-label");
    label->SetText(text);
    if (tooltip && tooltip[0] != '\0')
        label->SetTooltip(tooltip);
    Label* raw = label.get();
    cell->AddChild(std::move(label));
    row->AddChild(std::move(cell));
    return raw;
}

inline UIElement* AddFieldContainer(UIElement* row)
{
    if (!row)
        return nullptr;
    auto field = std::make_unique<UIElement>();
    field->AddClass("inspector-field");
    UIElement* raw = field.get();
    row->AddChild(std::move(field));
    return raw;
}

// A row that holds one action in the value column: an empty label cell and one field cell. Adds
// `rowClass` to the row and returns the field cell, whose parent is the row.
inline UIElement* AddActionRow(UIElement* parent, const char* rowClass)
{
    UIElement* row = AddRow(parent);
    if (!row)
        return nullptr;
    row->AddClass(rowClass);
    AddLabel(row, "");
    return AddFieldContainer(row);
}

// Enables or disables every inspector-label under `node`, so a row's label reads with its row.
inline void SetRowLabelsEnabled(UIElement* node, bool enabled)
{
    for (const auto& child : node->GetChildren())
    {
        if (child->HasClass("inspector-label"))
            child->SetEnabled(enabled);
        SetRowLabelsEnabled(child.get(), enabled);
    }
}

// Makes the inspector row holding `control` active or inactive. The disabled row is what blocks
// input to everything in it, label scrub included (the UI manager delivers no press, key or focus
// into a disabled element). The control and the row's labels are disabled too only for their look
// (Toggle.css, FloatField.css, inspector.css: .inspector-label:disabled, .inspector-row:disabled),
// which keys off each element's own state.
inline void SetRowOfControlEnabled(UIElement* control, bool enabled)
{
    if (!control)
        return;
    control->SetEnabled(enabled);
    for (UIElement* up = control->GetParent(); up; up = up->GetParent())
    {
        if (up->HasClass("inspector-row"))
        {
            up->SetEnabled(enabled);
            SetRowLabelsEnabled(up, enabled);
            break;
        }
    }
}

// Makes the inspector row holding `control` inactive (SetRowOfControlEnabled). A non-empty `reason`
// becomes the control's tooltip.
inline void DisableRowOfControl(UIElement* control, const char* reason = nullptr)
{
    if (!control)
        return;
    SetRowOfControlEnabled(control, false);
    if (reason)
        control->SetTooltip(reason);
}

inline FloatField* AddFloat(UIElement* parent, float value)
{
    auto field = std::make_unique<FloatField>();
    auto* raw = field.get();
    raw->SetValue(value);
    parent->AddChild(std::move(field));
    return raw;
}

inline IntField* AddInt(UIElement* parent, int value)
{
    auto field = std::make_unique<IntField>();
    auto* raw = field.get();
    raw->SetValue(value);
    parent->AddChild(std::move(field));
    return raw;
}

inline Toggle* AddToggle(UIElement* parent, bool value)
{
    auto toggle = std::make_unique<Toggle>();
    auto* raw = toggle.get();
    raw->SetValue(value);
    parent->AddChild(std::move(toggle));
    return raw;
}

inline TextField* AddTextRow(UIElement* parent, const std::string& labelText,
                             const std::string& initial, const char* tooltip = nullptr)
{
    auto* row = AddRow(parent);
    AddLabel(row, labelText, tooltip);
    auto field = std::make_unique<TextField>();
    field->SetValue(initial);
    field->AddClass("inspector-text-field");
    if (tooltip && tooltip[0] != '\0')
        field->SetTooltip(tooltip);
    auto* result = field.get();
    AddFieldContainer(row)->AddChild(std::move(field));
    return result;
}

inline Dropdown* AddDropdownRow(UIElement* parent, const std::string& labelText,
                                const std::vector<Dropdown::Option>& options, int selectedIndex,
                                const char* tooltip = nullptr)
{
    UIElement* row = AddRow(parent);
    AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = AddFieldContainer(row);

    auto dropdown = std::make_unique<Dropdown>();
    dropdown->SetOptions(options, selectedIndex);
    dropdown->AddClass("inspector-dropdown");
    Dropdown* raw = dropdown.get();
    fieldContainer->AddChild(std::move(dropdown));
    return raw;
}

template <typename T, size_t N>
EnumField<T>* AddEnumRow(UIElement* parent, const std::string& labelText,
                         const EnumEntry<T> (&entries)[N], T currentValue,
                         const char* tooltip = nullptr)
{
    UIElement* row = AddRow(parent);
    AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = AddFieldContainer(row);

    auto field = std::make_unique<EnumField<T>>();
    field->SetEntries(entries, currentValue);
    field->GetDropdown()->AddClass("inspector-dropdown");
    EnumField<T>* raw = field.get();
    fieldContainer->AddChild(std::move(field));
    return raw;
}

inline AssetField* AddAssetFieldRow(UIElement* parent,
                                    const std::string& labelText,
                                    const GUID& initial,
                                    const std::vector<AssetType>& acceptedTypes,
                                    AssetRegistry* registry,
                                    std::function<void(const GUID&)> onChanged,
                                    IThumbnailProvider* thumbnails = nullptr,
                                    const char* tooltip = nullptr)
{
    UIElement* row = AddRow(parent);
    AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = AddFieldContainer(row);

    auto field = std::make_unique<AssetField>();
    field->AddClass("dropdown-asset-field");
    field->SetAcceptedTypes(acceptedTypes);
    field->SetAssetRegistry(registry);
    if (thumbnails)
        field->SetThumbnailProvider(thumbnails);
    field->SetValue(initial);
    if (tooltip)
        field->SetTooltip(tooltip);
    field->SetOnValueChanged(std::move(onChanged));
    AssetField* raw = field.get();
    fieldContainer->AddChild(std::move(field));
    return raw;
}

inline EntityField* AddEntityFieldRow(UIElement* parent, const std::string& labelText,
                                      ECS::EntityHandle initial, ECS::World* world,
                                      std::function<void(ECS::EntityHandle)> onChanged,
                                      const std::vector<ECS::ComponentTypeId>& required = {},
                                      const char* tooltip = nullptr)
{
    UIElement* row = AddRow(parent);
    AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = AddFieldContainer(row);

    auto field = std::make_unique<EntityField>();
    field->SetWorld(world);
    if (!required.empty())
        field->SetRequiredComponents(required);
    field->SetValue(initial);
    if (tooltip)
        field->SetTooltip(tooltip);
    field->SetOnValueChanged(std::move(onChanged));
    EntityField* raw = field.get();
    fieldContainer->AddChild(std::move(field));
    return raw;
}

inline std::string AssetPathExtensionText(std::string_view text)
{
    const size_t slash = text.find_last_of("/\\");
    const size_t searchStart = (slash == std::string_view::npos) ? 0 : slash + 1;
    const size_t dot = text.find_last_of('.');
    if (dot == std::string_view::npos || dot < searchStart || dot + 1 >= text.size())
        return "File Extension: (none)";
    return "File Extension: " + std::string(text.substr(dot));
}

inline void AddTextBlock(UIElement* parent, const std::string& text, const char* cssClass = "inspector-text")
{
    if (!parent)
        return;

    if (cssClass && std::string_view(cssClass) == "inspector-asset-path")
    {
        auto clip = std::make_unique<UIElement>();
        clip->AddClass("inspector-asset-path-clip");
        clip->SetTooltip(text);

        auto label = std::make_unique<Label>();
        label->AddClass(cssClass);
        label->SetText(AssetPathExtensionText(text));
        clip->AddChild(std::move(label));

        parent->AddChild(std::move(clip));
        return;
    }

    auto label = std::make_unique<Label>();
    if (cssClass && cssClass[0] != '\0')
    {
        label->AddClass(cssClass);
    }
    label->SetText(text);
    parent->AddChild(std::move(label));
}

inline void AddSelectableTextBlock(UIElement* parent, const std::string& text, const char* cssClass = "inspector-text")
{
    if (!parent)
        return;

    auto ta = std::make_unique<TextArea>();
    ta->SetReadOnly(true);
    if (cssClass && cssClass[0] != '\0')
    {
        ta->AddClass(cssClass);
    }
    ta->SetValue(text);
    parent->AddChild(std::move(ta));
}

inline void AddLine(UIElement* parent, const std::string& text)
{
    AddTextBlock(parent, text, "inspector-text");
}

// Names the file an asset's source lives in. The inspector column leaves ~215px of content width
// once a code gutter is subtracted, which is too narrow to read source in — a wrapped preview there
// runs 3-4 words per line and its gutter numbers stop lining up with the visual rows. Show the path
// and let the Open buttons above do the reading.
inline void AddSourceFileRow(UIElement* parent, const std::filesystem::path& sourcePath)
{
    if (!parent)
        return;

    AddTextBlock(parent, "Source:", "inspector-section-subheader");
    AddTextBlock(parent, sourcePath.string(), "inspector-source-path");
}

// Explanatory copy in the panel's info-card treatment. A card reads as a note ABOUT the rows near
// it, which is what this kind of text is; loose text reads as one more row and competes with the
// controls it explains. Use AddLine for a plain line and EditorUI::InspectorNotice for a problem.
inline void AddInfoCard(UIElement* parent, const std::string& text)
{
    if (!parent)
        return;
    parent->AddChild(std::make_unique<EditorUI::CollapsibleInfoCard>(text));
}

inline const char* BoolToString(bool v) { return v ? "true" : "false"; }

inline std::string ToHex(uint32_t v)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v << std::dec;
    return oss.str();
}
/// Returns true if the given UIElement (or any descendant) holds the current keyboard focus.
/// Used by inspector RefreshFromWorld() methods to avoid overwriting fields the user is editing.
inline bool ContainsFocusedElement(const UIElement* root, const std::string& focusId)
{
    if (!root || focusId.empty())
        return false;
    if (root->IsFocusTargetForId(focusId))
        return true;
    for (const auto& ch : root->GetChildren())
    {
        if (ContainsFocusedElement(ch.get(), focusId))
            return true;
    }
    return false;
}

} // namespace GameEngine::InspectorUI

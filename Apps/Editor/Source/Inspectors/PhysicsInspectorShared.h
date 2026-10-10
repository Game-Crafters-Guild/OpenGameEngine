#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "InspectorRegistry.h"

#include "UI/UIElement.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"

namespace GameEngine::Editor
{
class UndoRedoService;
class EditorChangeNotifications;
} // namespace GameEngine::Editor

namespace GameEngine
{
namespace InspectorPhysicsUI
{
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

inline FloatField* AddFloat(UIElement* parent, float v)
{
    auto f = std::make_unique<FloatField>();
    auto* raw = f.get();
    raw->SetValue(v);
    parent->AddChild(std::move(f));
    return raw;
}

inline IntField* AddInt(UIElement* parent, int v)
{
    auto f = std::make_unique<IntField>();
    auto* raw = f.get();
    raw->SetValue(v);
    parent->AddChild(std::move(f));
    return raw;
}

inline Toggle* AddToggle(UIElement* parent, bool v)
{
    auto t = std::make_unique<Toggle>();
    auto* raw = t.get();
    raw->SetValue(v);
    parent->AddChild(std::move(t));
    return raw;
}

template <typename T>
inline void WriteBytes(std::vector<std::uint8_t>& out, const T& v)
{
    out.resize(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));
}

template <typename T>
inline bool ReadBytes(const std::vector<std::uint8_t>& in, T& out)
{
    if (in.size() != sizeof(T))
        return false;
    std::memcpy(&out, in.data(), sizeof(T));
    return true;
}

} // namespace InspectorPhysicsUI
} // namespace GameEngine

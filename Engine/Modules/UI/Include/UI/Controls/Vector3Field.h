#pragma once

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "Platform/SystemMetrics.h"
#include "UI/Controls/BaseField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "Rendering/Common/Math.h"

namespace GameEngine {

// Typed field for editing a Rendering::Vector3 value via three TextFields (X, Y, Z).
class Vector3Field : public Field<Rendering::Vector3>
{
public:
    using ValueType = Rendering::Vector3;

    Vector3Field();

    // Override to keep child TextFields in sync with the stored value.
    void SetValue(const ValueType& v) override;

    // Give stable ids to the component FloatFields (X/Y/Z) for automation and tooling.
    // This is optional; if not called, component ids remain auto-generated.
    void ConfigureComponentIds(const std::string& baseId);

    /** Set value used when user double-clicks a component label (X/Y/Z) without dragging. Default (0,0,0). */
    void SetDefaultValue(const ValueType& v) { m_DefaultValue = v; }
    const ValueType& GetDefaultValue() const { return m_DefaultValue; }

    // Clamp every component's user edits to [min, max] (see FloatField::SetValueRange).
    // Fields are pooled and rebound, so every bind must set or clear the range.
    void SetComponentValueRange(float min, float max);
    void ClearComponentValueRange();

    // Inspector Vector3 fields scrub from the X/Y/Z labels. Graph node fields
    // scrub the numeric boxes instead; labels stay clickable for double-click reset.
    void SetLabelDragEnabled(bool enabled);
    bool IsLabelDragEnabled() const { return m_LabelDragEnabled; }
    void EnableComponentDragToChange();

private:
    void OnComponentChanged(bool isFinal);
    void SetupLabelDrag(Label* label, FloatField* field);

    Label* m_LabelX = nullptr;
    Label* m_LabelY = nullptr;
    Label* m_LabelZ = nullptr;
    FloatField* m_X = nullptr;
    FloatField* m_Y = nullptr;
    FloatField* m_Z = nullptr;
    bool m_SuppressCallbacks = false;
    bool m_LabelDragEnabled = true;

    ValueType m_DefaultValue{0.0f, 0.0f, 0.0f};

    // Drag-to-adjust state
    Label* m_DraggingLabel = nullptr;
    FloatField* m_DraggingField = nullptr;
    float m_DragStartX = 0.0f;
    float m_DragStartValue = 0.0f;
    bool m_LabelDragDidMove = false;
    std::chrono::steady_clock::time_point m_LastLabelMouseDownTime{};
};

inline Vector3Field::Vector3Field()
{
    AddClass("vector3-field");

    // X
    auto labelX = std::make_unique<Label>();
    m_LabelX = labelX.get();
    m_LabelX->AddClass("vector3-label");
    m_LabelX->AddClass("vector3-label-x");
    m_LabelX->SetText("X");
    AddChild(std::move(labelX));

    auto x = std::make_unique<FloatField>();
    m_X = x.get();
    m_X->AddClass("vector3-component-x");
    m_X->SetOnValueChanging([this](const float&) { OnComponentChanged(false); });
    m_X->SetOnValueChanged([this](const float&) { OnComponentChanged(true); });
    AddChild(std::move(x));
    SetupLabelDrag(m_LabelX, m_X);

    // Y
    auto labelY = std::make_unique<Label>();
    m_LabelY = labelY.get();
    m_LabelY->AddClass("vector3-label");
    m_LabelY->AddClass("vector3-label-y");
    m_LabelY->SetText("Y");
    AddChild(std::move(labelY));

    auto y = std::make_unique<FloatField>();
    m_Y = y.get();
    m_Y->AddClass("vector3-component-y");
    m_Y->SetOnValueChanging([this](const float&) { OnComponentChanged(false); });
    m_Y->SetOnValueChanged([this](const float&) { OnComponentChanged(true); });
    AddChild(std::move(y));
    SetupLabelDrag(m_LabelY, m_Y);

    // Z
    auto labelZ = std::make_unique<Label>();
    m_LabelZ = labelZ.get();
    m_LabelZ->AddClass("vector3-label");
    m_LabelZ->AddClass("vector3-label-z");
    m_LabelZ->SetText("Z");
    AddChild(std::move(labelZ));

    auto z = std::make_unique<FloatField>();
    m_Z = z.get();
    m_Z->AddClass("vector3-component-z");
    m_Z->SetOnValueChanging([this](const float&) { OnComponentChanged(false); });
    m_Z->SetOnValueChanged([this](const float&) { OnComponentChanged(true); });
    AddChild(std::move(z));
    SetupLabelDrag(m_LabelZ, m_Z);

    // Initialize with zero vector.
    SetValue(ValueType(0.0f, 0.0f, 0.0f));
}

inline void Vector3Field::SetValue(const ValueType& v)
{
    m_SuppressCallbacks = true;
    Field<Rendering::Vector3>::SetValue(v);

    if (m_X) m_X->SetValue(v.x);
    if (m_Y) m_Y->SetValue(v.y);
    if (m_Z) m_Z->SetValue(v.z);

    m_SuppressCallbacks = false;
}

inline void Vector3Field::OnComponentChanged(bool isFinal)
{
    if (m_SuppressCallbacks)
    {
        return;
    }

    float x = m_Value.x;
    float y = m_Value.y;
    float z = m_Value.z;

    if (m_X)
    {
        x = m_X->GetValue();
    }

    if (m_Y)
    {
        y = m_Y->GetValue();
    }

    if (m_Z)
    {
        z = m_Z->GetValue();
    }

    ValueType newValue(x, y, z);

    if (newValue.x != m_Value.x || newValue.y != m_Value.y || newValue.z != m_Value.z)
    {
        m_Value = newValue;
        MarkDirty(LayoutDirty | VisualDirty);
        if (isFinal)
        {
            NotifyValueChanged();
        }
        else
        {
            NotifyValueChanging();
        }
    }
    else if (isFinal)
    {
        // For final changes, still notify so listeners can react to commit.
        NotifyValueChanged();
    }
}

inline void Vector3Field::ConfigureComponentIds(const std::string& baseId)
{
    if (baseId.empty())
        return;
    // Keep the Vector3Field itself addressable too (useful for hit-targeting).
    SetId(baseId);
    if (m_X) m_X->SetId(baseId + ".X");
    if (m_Y) m_Y->SetId(baseId + ".Y");
    if (m_Z) m_Z->SetId(baseId + ".Z");
}

inline void Vector3Field::SetComponentValueRange(float min, float max)
{
    if (m_X) m_X->SetValueRange(min, max);
    if (m_Y) m_Y->SetValueRange(min, max);
    if (m_Z) m_Z->SetValueRange(min, max);
}

inline void Vector3Field::ClearComponentValueRange()
{
    if (m_X) m_X->ClearValueRange();
    if (m_Y) m_Y->ClearValueRange();
    if (m_Z) m_Z->ClearValueRange();
}

inline void Vector3Field::SetLabelDragEnabled(bool enabled)
{
    m_LabelDragEnabled = enabled;
    auto apply = [enabled](Label* label)
    {
        if (!label)
            return;
        if (enabled)
            label->AddClass("draggable-label");
        else
            label->RemoveClass("draggable-label");
    };
    apply(m_LabelX);
    apply(m_LabelY);
    apply(m_LabelZ);
}

inline void Vector3Field::EnableComponentDragToChange()
{
    if (m_X)
        m_X->EnableDragToChange();
    if (m_Y)
        m_Y->EnableDragToChange();
    if (m_Z)
        m_Z->EnableDragToChange();
}

inline void Vector3Field::SetupLabelDrag(Label* label, FloatField* field)
{
    if (!label || !field)
        return;

    // Set cursor to indicate draggable
    label->AddClass("draggable-label");

    // Mouse down: detect double-click (second press within 500ms) to reset this component, else start drag
    const bool isX = (field == m_X);
    const bool isY = (field == m_Y);
    const bool isZ = (field == m_Z);
    label->RegisterEventHandler(kEventMouseDown, [this, label, field, isX, isY, isZ](UIEvent& e) {
        if (e.Button == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastLabelMouseDownTime);
            if (elapsed < Platform::GetDoubleClickInterval()) {
                ValueType v = GetValue();
                if (isX) v.x = m_DefaultValue.x;
                else if (isY) v.y = m_DefaultValue.y;
                else if (isZ) v.z = m_DefaultValue.z;
                SetValue(v);
                NotifyValueChanged();
                m_LastLabelMouseDownTime = std::chrono::steady_clock::time_point{};
                e.Stop();
                return;
            }
            m_LastLabelMouseDownTime = now;
            if (!m_LabelDragEnabled)
            {
                e.Stop();
                return;
            }
            if (m_DraggingLabel != nullptr)
                return;
            m_LabelDragDidMove = false;
            m_DraggingLabel = label;
            m_DraggingField = field;
            m_DragStartX = e.X;
            m_DragStartValue = field->GetValue();
            label->AddClass("dragging");
            e.Capture(label);
            e.Stop();
        }
    });

    // Mouse move: adjust value; only count as "drag" if moved past threshold (avoids jitter blocking double-click reset)
    constexpr float kDragThresholdPx = 3.0f;
    label->RegisterEventHandler(kEventMouseMove, [this, field](UIEvent& e) {
        if (m_DraggingLabel && m_DraggingField == field) {
            float deltaX = e.X - m_DragStartX;
            if (std::fabs(deltaX) > kDragThresholdPx)
                m_LabelDragDidMove = true;
            float sensitivity = 0.1f;
            float newValue = m_DragStartValue + deltaX * sensitivity;
            // The label scrub writes the component directly, so it has to apply
            // the component's range itself the way FloatField's own scrub does.
            field->SetValue(field->ClampToValueRange(newValue));
            OnComponentChanged(false);
            e.Stop();
        }
    });

    // Mouse up: end drag and commit
    label->RegisterEventHandler(kEventMouseUp, [this, label, field](UIEvent& e) {
        if (m_DraggingLabel && m_DraggingField == field && e.Button == 0) {
            label->RemoveClass("dragging");
            m_DraggingLabel = nullptr;
            m_DraggingField = nullptr;
            OnComponentChanged(true);
            e.Stop();
        }
    });
}

} // namespace GameEngine

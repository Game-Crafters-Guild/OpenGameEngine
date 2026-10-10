#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/AssetField.h"
#include "UI/EntityField.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/TextField.h"
#include "Inspectors/InspectorUIHelpers.h"

namespace GameEngine
{
namespace InspectorDrag
{


// ============================================================================
// Drag state (singleton for one active drag at a time)
// ============================================================================

struct DragState
{
    Label* draggingLabel = nullptr;
    FloatField* draggingFloatField = nullptr;
    IntField* draggingIntField = nullptr;
    Toggle* draggingToggle = nullptr;
    Slider* draggingSlider = nullptr;
    float dragStartX = 0.0f;
    float dragStartFloatValue = 0.0f;
    int dragStartIntValue = 0;
    bool dragDidMove = false;
};

inline DragState& GetDragState()
{
    static DragState state;
    return state;
}

// Inspector label-drag tuning (shared by component inspectors, material rows, script vars).
// Lower float/int sensitivity = slower value change per pixel (finer control).
// Larger slider divisor = more horizontal drag needed to sweep min..max on range sliders.
inline constexpr float kInspectorDragFloatSensitivity = 0.03f;
inline constexpr float kInspectorDragIntSensitivity = 0.035f;
inline constexpr float kInspectorDragSliderPixelsPerFullRange = 480.0f;
// 60px with the field's side padding fits seven glyphs ("12.558"); at 50px a
// typical value overflowed and edit mode's caret scroll made the text jump.
inline constexpr float kInspectorSliderValueFieldWidthPx = 60.0f;
inline constexpr float kInspectorSliderValueRightInsetPx = 0.0f;
// A slider that shares its row with a value field may shrink further than a
// lone slider: inside a group foldout's indent the pair otherwise overflows
// the row and the field clips at the panel edge. The field keeps its fixed
// width — the number must stay readable; the track gives.
inline constexpr float kInspectorSliderWithValueMinWidthPx = 48.0f;
inline constexpr float kInspectorSliderTrackPaddingPx = 7.0f;
inline constexpr float kInspectorSliderMinWidthPx = 80.0f;
inline constexpr float kInspectorWideSliderMinWidthPx = 120.0f;

inline void ApplyInspectorSliderTrackInset(Slider* slider)
{
    if (slider)
        slider->SetTrackPaddingPx(kInspectorSliderTrackPaddingPx);
}

inline void ApplyInspectorSliderStyle(Slider* slider, float minWidthPx = kInspectorSliderMinWidthPx)
{
    if (!slider)
        return;

    slider->AddClass("property-slider");
    ApplyInspectorSliderTrackInset(slider);
    slider->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(minWidthPx));
}

inline Slider* AddInspectorSlider(
    UIElement* parent,
    float initial,
    float min,
    float max,
    float step = 0.0f,
    bool showValueBubble = true,
    float minWidthPx = kInspectorSliderMinWidthPx)
{
    if (!parent)
        return nullptr;

    auto slider = std::make_unique<Slider>();
    Slider* sliderRaw = slider.get();
    ApplyInspectorSliderStyle(sliderRaw, minWidthPx);
    sliderRaw->SetMin(min);
    sliderRaw->SetMax(max);
    sliderRaw->SetStep(step);
    sliderRaw->SetShowValueBubble(showValueBubble);
    sliderRaw->SetValueWithoutNotify(initial);
    parent->AddChild(std::move(slider));
    return sliderRaw;
}

// Find the existing layout row that should carry the transient .dragging state.
// Inspector labels may be wrapped in a label cell; popover labels are direct
// row children, so search a few ancestors for either row shape.
inline UIElement* FindDragHighlightRow(Label* label)
{
    constexpr int kMaxAncestorDepth = 4;
    UIElement* el = label ? label->GetParent() : nullptr;
    for (int depth = 0; el && depth < kMaxAncestorDepth; el = el->GetParent(), ++depth)
    {
        if (el->HasClass("inspector-row") || el->HasClass("popover-row"))
            return el;
    }
    return nullptr;
}

// Toggle the scrub-drag highlight: the label brightens and the enclosing row
// gets one .dragging class so CSS highlights every value field inside it,
// independent of the row's internal structure.
inline void SetLabelDragHighlight(Label* label, bool on)
{
    if (!label)
        return;

    const unsigned flags = UIElement::StyleDirty | UIElement::VisualDirty;
    std::function<void(UIElement*)> markSubtree = [&markSubtree, flags](UIElement* el) {
        if (!el)
            return;
        el->MarkDirty(flags);
        for (const auto& ch : el->GetChildren())
            markSubtree(ch.get());
    };

    if (on)
    {
        label->AddClass("dragging");
    }
    else
    {
        label->RemoveClass("dragging");
    }
    label->MarkDirty(flags);

    if (UIElement* row = FindDragHighlightRow(label))
    {
        if (on)
        {
            row->AddClass("dragging");
        }
        else
        {
            row->RemoveClass("dragging");
        }
        markSubtree(row);
    }
}

// Clear label/slider/toggle drag state (e.g. on mouse up anywhere in the row to avoid stuck state)
inline void ClearLabelDragState()
{
    auto& state = GetDragState();
    if (state.draggingLabel)
    {
        SetLabelDragHighlight(state.draggingLabel, false);
        state.draggingLabel = nullptr;
    }
    state.draggingSlider = nullptr;
    state.draggingToggle = nullptr;
    state.draggingFloatField = nullptr;
    state.draggingIntField = nullptr;
    state.dragDidMove = false;
}

// Finish the active label drag if it changed a value, then clear the shared state.
// Used when an inspector rebuild or a new mouse-down interrupts a drag before the
// captured label receives its MouseUp.
inline void FinalizeAndClearLabelDragState()
{
    auto& state = GetDragState();
    const bool didMove = state.dragDidMove;
    Slider* slider = state.draggingSlider;
    FloatField* floatField = state.draggingFloatField;
    IntField* intField = state.draggingIntField;
    ClearLabelDragState();

    if (didMove)
    {
        if (slider)
            slider->NotifyValueChanged();
        else if (floatField)
            floatField->NotifyValueChanged();
        else if (intField)
            intField->NotifyValueChanged();
    }
}

inline bool IsLabelDragDisabled(Label* label)
{
    return !label || label->HasClass("inspector-label-no-drag");
}

// A label scrub writes straight into the control beside it, so a disabled
// control has to disable the scrub as well — otherwise "this field refuses
// input" stops at the field box and the label next to it still edits the
// value. Checked per press, not once at wiring time: a row can be gated on a
// sibling and flip while the panel is open.
inline bool IsLabelDragTargetDisabled(const UIElement* target)
{
    return !target || !target->IsEnabled();
}

inline void MarkLabelDraggable(Label* label)
{
    if (IsLabelDragDisabled(label))
        return;

    label->AddClass("inspector-label-draggable");
    if (UIElement* cell = label->GetParent(); cell && cell->HasClass("inspector-label-cell"))
        cell->AddClass("inspector-label-cell-draggable");
}

// ============================================================================
// Drag-paint toggle groups
// ============================================================================

// Reusable inspector interaction for compact boolean controls such as visibility
// dots. Pressing one control toggles it and establishes the value for the gesture;
// dragging across other registered controls paints that same value once per item.
// The begin/end callbacks let callers coalesce the whole gesture into one undo step.
class ToggleDragPaintGroup;

class DragPaintToggle final : public ToggleBase
{
public:
    using ItemId = size_t;

    DragPaintToggle(std::shared_ptr<ToggleDragPaintGroup> group, ItemId itemId)
        : m_Group(std::move(group)), m_ItemId(itemId)
    {
    }
    ~DragPaintToggle() override;

    void OnEvent(UIEvent& e) override;

private:
    std::shared_ptr<ToggleDragPaintGroup> m_Group;
    ItemId m_ItemId = 0;
};

class ToggleDragPaintGroup final : public std::enable_shared_from_this<ToggleDragPaintGroup>
{
public:
    using ItemId = DragPaintToggle::ItemId;
    using BeginGestureFn = std::function<void(bool)>;
    using ApplyFn = std::function<void(ItemId, bool, ToggleBase&)>;
    using EndGestureFn = std::function<void()>;

    ToggleDragPaintGroup(BeginGestureFn beginGesture, ApplyFn apply, EndGestureFn endGesture)
        : m_BeginGesture(std::move(beginGesture))
        , m_Apply(std::move(apply))
        , m_EndGesture(std::move(endGesture))
    {
    }

    std::unique_ptr<DragPaintToggle> CreateToggle(ItemId itemId, bool initialValue)
    {
        auto toggle = std::make_unique<DragPaintToggle>(shared_from_this(), itemId);
        DragPaintToggle* raw = toggle.get();
        raw->SetValueWithoutNotify(initialValue);
        m_Items.push_back({itemId, raw});
        return toggle;
    }

    void Unregister(DragPaintToggle* toggle)
    {
        if (!toggle)
            return;
        if (m_Dragging && m_Source == toggle)
            FinishGesture();
        std::erase_if(m_Items, [toggle](const Item& item) { return item.Toggle == toggle; });
    }

    void HandleEvent(DragPaintToggle& source, ItemId itemId, UIEvent& e)
    {
        if (e.Id == kEventMouseLeave)
        {
            source.RemoveClass(kHoverPreviewSuppressedClass);
            return;
        }

        if (e.Id == kEventMouseDown && e.Button == 0)
        {
            if (m_Dragging)
                FinishGesture();

            m_Dragging = true;
            m_Source = &source;
            m_TargetValue = !source.GetValue();
            m_Applied.clear();
            if (m_BeginGesture)
                m_BeginGesture(m_TargetValue);
            ApplyItem(itemId);
            e.Capture(&source);
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseMove && m_Dragging)
        {
            if (UIManager* ui = source.GetOwnerManager(); ui && !ui->IsMouseDown())
            {
                FinishGesture(e.X, e.Y);
                return;
            }

            for (const Item& item : m_Items)
            {
                if (!item.Toggle || m_Applied.contains(item.Id))
                    continue;
                if (!ContainsPoint(*item.Toggle, e.X, e.Y))
                    continue;
                ApplyItem(item.Id);
                break;
            }
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseUp && e.Button == 0 && m_Dragging)
        {
            FinishGesture(e.X, e.Y);
            e.Stop();
            return;
        }

        if (e.Id == kEventKeyDown &&
            (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter))
        {
            const bool targetValue = !source.GetValue();
            if (m_BeginGesture)
                m_BeginGesture(targetValue);
            m_TargetValue = targetValue;
            m_Applied.clear();
            ApplyItem(itemId);
            if (m_EndGesture)
                m_EndGesture();
            m_Applied.clear();
            source.RemoveClass(kHoverPreviewSuppressedClass);
            e.Stop();
            return;
        }
    }

    static constexpr const char* kHoverPreviewSuppressedClass =
        "inspector-toggle-hover-preview-suppressed";

private:
    struct Item
    {
        ItemId Id = 0;
        DragPaintToggle* Toggle = nullptr;
    };

    static bool ContainsPoint(const UIElement& element, float x, float y)
    {
        float left = 0.0f;
        float top = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        element.GetHitTestBounds(left, top, width, height);
        return x >= left && x < left + width && y >= top && y < top + height;
    }

    void ApplyItem(ItemId itemId)
    {
        const auto it = std::find_if(m_Items.begin(), m_Items.end(),
            [itemId](const Item& item) { return item.Id == itemId; });
        if (it == m_Items.end() || !it->Toggle || m_Applied.contains(itemId))
            return;

        m_Applied.insert(itemId);
        it->Toggle->SetValueWithoutNotify(m_TargetValue);
        it->Toggle->AddClass(kHoverPreviewSuppressedClass);
        if (m_Apply)
            m_Apply(itemId, m_TargetValue, *it->Toggle);
    }

    void FinishGesture(float pointerX = std::numeric_limits<float>::quiet_NaN(),
                       float pointerY = std::numeric_limits<float>::quiet_NaN())
    {
        if (!m_Dragging)
            return;

        m_Dragging = false;
        m_Source = nullptr;
        if (m_EndGesture)
            m_EndGesture();
        m_Applied.clear();

        const bool hasPointer = !std::isnan(pointerX) && !std::isnan(pointerY);
        for (const Item& item : m_Items)
        {
            if (item.Toggle && (!hasPointer || !ContainsPoint(*item.Toggle, pointerX, pointerY)))
                item.Toggle->RemoveClass(kHoverPreviewSuppressedClass);
        }
    }

    std::vector<Item> m_Items;
    std::unordered_set<ItemId> m_Applied;
    BeginGestureFn m_BeginGesture;
    ApplyFn m_Apply;
    EndGestureFn m_EndGesture;
    bool m_Dragging = false;
    bool m_TargetValue = false;
    DragPaintToggle* m_Source = nullptr;
};

inline DragPaintToggle::~DragPaintToggle()
{
    if (m_Group)
        m_Group->Unregister(this);
}

inline void DragPaintToggle::OnEvent(UIEvent& e)
{
    if (m_Group)
    {
        m_Group->HandleEvent(*this, m_ItemId, e);
        if (e.Handled)
            return;
    }
    ToggleBase::OnEvent(e);
}

// ============================================================================
// Drag-to-adjust setup functions
// ============================================================================

// Set up drag-to-adjust on a label for a FloatField. Double-click on label resets to defaultValue.
inline void SetupLabelDragFloat(Label* label, FloatField* field,
                                std::function<void()> onChanging = nullptr,
                                std::function<void()> onChanged = nullptr,
                                float defaultValue = 0.0f,
                                float minValue = -std::numeric_limits<float>::infinity(),
                                float maxValue = std::numeric_limits<float>::infinity())
{
    if (IsLabelDragDisabled(label) || !field)
        return;

    MarkLabelDraggable(label);

    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    auto dragDidMove = std::make_shared<bool>(false);

    label->RegisterEventHandler(kEventMouseDown, [label, field, defaultValue, minValue, maxValue, onChanged, lastClickTime, dragDidMove](UIEvent& e) {
        if (IsLabelDragTargetDisabled(field))
            return;
        if (e.Button == 0) {
            auto& state = GetDragState();
            if (state.draggingLabel != nullptr)
                FinalizeAndClearLabelDragState();

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClickTime);
            if (lastClickTime->time_since_epoch().count() != 0 && elapsed < GameEngine::Platform::GetDoubleClickInterval()) {
                field->SetValue(std::clamp(defaultValue, minValue, maxValue));
                if (onChanged) onChanged();
                *lastClickTime = {};
                e.Stop();
                return;
            }
            *lastClickTime = now;
            *dragDidMove = false;

            state.draggingLabel = label;
            state.draggingFloatField = field;
            state.draggingIntField = nullptr;
            state.draggingToggle = nullptr;
            state.draggingSlider = nullptr;
            state.dragStartX = e.X;
            state.dragStartFloatValue = field->GetValue();
            state.dragDidMove = false;
            SetLabelDragHighlight(label, true);
            e.Capture(label);
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseMove, [label, field, minValue, maxValue, onChanging, onChanged, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        if (UIManager* mgr = label->GetOwnerManager())
        {
            if (!mgr->IsMouseDown())
            {
                const bool shouldCommit =
                    state.draggingLabel && state.draggingFloatField == field && *dragDidMove && onChanged;
                ClearLabelDragState();
                if (shouldCommit)
                    onChanged();
                return;
            }
        }
        if (state.draggingLabel && state.draggingFloatField == field) {
            float deltaX = e.X - state.dragStartX;
            float newValue = state.dragStartFloatValue + deltaX * kInspectorDragFloatSensitivity;
            newValue = std::clamp(newValue, minValue, maxValue);
            field->SetValue(newValue);
            *dragDidMove = true;
            state.dragDidMove = true;
            if (onChanging) onChanging();
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseUp, [label, field, onChanged, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        if (state.draggingLabel && state.draggingFloatField == field && e.Button == 0) {
            SetLabelDragHighlight(label, false);
            state.draggingLabel = nullptr;
            state.draggingFloatField = nullptr;
            state.dragDidMove = false;

            if (*dragDidMove) {
                if (onChanged) onChanged();
            }
            e.Stop();
        }
    });
}

// Set up drag-to-adjust on a label for an IntField. Double-click on label resets to defaultValue.
inline void SetupLabelDragInt(Label* label, IntField* field,
                              std::function<void()> onChanging = nullptr,
                              std::function<void()> onChanged = nullptr,
                              int defaultValue = 0)
{
    if (IsLabelDragDisabled(label) || !field)
        return;

    MarkLabelDraggable(label);

    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    auto dragDidMove = std::make_shared<bool>(false);

    label->RegisterEventHandler(kEventMouseDown, [label, field, defaultValue, onChanged, lastClickTime, dragDidMove](UIEvent& e) {
        if (IsLabelDragTargetDisabled(field))
            return;
        if (e.Button == 0) {
            auto& state = GetDragState();
            if (state.draggingLabel != nullptr)
                FinalizeAndClearLabelDragState();

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClickTime);
            if (lastClickTime->time_since_epoch().count() != 0 && elapsed < GameEngine::Platform::GetDoubleClickInterval()) {
                field->SetValue(defaultValue);
                if (onChanged) onChanged();
                *lastClickTime = {};
                e.Stop();
                return;
            }
            *lastClickTime = now;
            *dragDidMove = false;

            state.draggingLabel = label;
            state.draggingFloatField = nullptr;
            state.draggingIntField = field;
            state.draggingToggle = nullptr;
            state.draggingSlider = nullptr;
            state.dragStartX = e.X;
            state.dragStartIntValue = field->GetValue();
            state.dragDidMove = false;
            SetLabelDragHighlight(label, true);
            e.Capture(label);
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseMove, [label, field, onChanging, onChanged, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        if (UIManager* mgr = label->GetOwnerManager())
        {
            if (!mgr->IsMouseDown())
            {
                const bool shouldCommit =
                    state.draggingLabel && state.draggingIntField == field && *dragDidMove && onChanged;
                ClearLabelDragState();
                if (shouldCommit)
                    onChanged();
                return;
            }
        }
        if (state.draggingLabel && state.draggingIntField == field) {
            float deltaX = e.X - state.dragStartX;
            int newValue = state.dragStartIntValue +
                           static_cast<int>(std::round(deltaX * kInspectorDragIntSensitivity));
            field->SetValue(newValue);
            *dragDidMove = true;
            state.dragDidMove = true;
            if (onChanging) onChanging();
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseUp, [label, field, onChanged, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        if (state.draggingLabel && state.draggingIntField == field && e.Button == 0) {
            SetLabelDragHighlight(label, false);
            state.draggingLabel = nullptr;
            state.draggingIntField = nullptr;
            state.dragDidMove = false;

            if (*dragDidMove) {
                if (onChanged) onChanged();
            }
            e.Stop();
        }
    });
}

// Set up drag on a label for a Toggle: drag right = on, drag left = off
inline void SetupLabelDragToggle(Label* label, Toggle* toggle,
                                 std::function<void()> onChanged = nullptr)
{
    if (IsLabelDragDisabled(label) || !toggle)
        return;

    MarkLabelDraggable(label);

    const float kDragThresholdPx = 20.0f;

    label->RegisterEventHandler(kEventMouseDown, [label, toggle](UIEvent& e) {
        if (IsLabelDragTargetDisabled(toggle))
            return;
        if (e.Button == 0) {
            auto& state = GetDragState();
            if (state.draggingLabel != nullptr)
                FinalizeAndClearLabelDragState();
            state.draggingLabel = label;
            state.draggingFloatField = nullptr;
            state.draggingIntField = nullptr;
            state.draggingToggle = toggle;
            state.draggingSlider = nullptr;
            state.dragStartX = e.X;
            state.dragDidMove = false;
            SetLabelDragHighlight(label, true);
            e.Capture(label);
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseMove, [label, toggle, kThreshold = kDragThresholdPx, onChanged](UIEvent& e) {
        auto& state = GetDragState();
        if (UIManager* mgr = label->GetOwnerManager())
        {
            if (!mgr->IsMouseDown())
            {
                ClearLabelDragState();
                return;
            }
        }
        if (state.draggingLabel && state.draggingToggle == toggle) {
            float deltaX = e.X - state.dragStartX;
            if (deltaX > kThreshold && !toggle->GetValue()) {
                toggle->SetValue(true);
                state.dragDidMove = true;
                if (onChanged) onChanged();
            } else if (deltaX < -kThreshold && toggle->GetValue()) {
                toggle->SetValue(false);
                state.dragDidMove = true;
                if (onChanged) onChanged();
            }
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseUp, [label, toggle](UIEvent& e) {
        auto& state = GetDragState();
        if (state.draggingLabel && state.draggingToggle == toggle && e.Button == 0) {
            SetLabelDragHighlight(label, false);
            state.draggingLabel = nullptr;
            state.draggingToggle = nullptr;
            state.dragDidMove = false;
        }
    });
}

// Set up drag on a label for a Slider: horizontal drag scrubs the value
inline void SetupLabelDragSlider(Label* label, Slider* slider,
                                 std::function<void()> onChanging = nullptr,
                                 std::function<void()> onChanged = nullptr,
                                 float defaultValue = std::numeric_limits<float>::quiet_NaN())
{
    if (IsLabelDragDisabled(label) || !slider)
        return;

    MarkLabelDraggable(label);
    const float resetTo = std::isnan(defaultValue) ? slider->GetValue() : defaultValue;
    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    auto dragDidMove = std::make_shared<bool>(false);

    label->RegisterEventHandler(kEventMouseDown, [label, slider, resetTo, onChanged, lastClickTime, dragDidMove](UIEvent& e) {
        if (IsLabelDragTargetDisabled(slider))
            return;
        if (e.Button == 0) {
            auto& state = GetDragState();
            if (state.draggingLabel != nullptr)
                FinalizeAndClearLabelDragState();

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClickTime);
            if (lastClickTime->time_since_epoch().count() != 0 && elapsed < GameEngine::Platform::GetDoubleClickInterval()) {
                const float clampedReset = std::max(slider->GetMin(), std::min(slider->GetMax(), resetTo));
                slider->SetValueWithoutNotify(clampedReset);
                slider->NotifyValueChanged();
                if (onChanged) onChanged();
                *lastClickTime = {};
                e.Stop();
                return;
            }
            *lastClickTime = now;
            *dragDidMove = false;

            state.draggingLabel = label;
            state.draggingFloatField = nullptr;
            state.draggingIntField = nullptr;
            state.draggingToggle = nullptr;
            state.draggingSlider = slider;
            state.dragStartX = e.X;
            state.dragStartFloatValue = slider->GetValue();
            state.dragDidMove = false;
            SetLabelDragHighlight(label, true);
            e.Capture(label);
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseMove, [label, slider, onChanging, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        // If mouse was released (e.g. MouseUp went to another element), clear and stop
        if (UIManager* mgr = label->GetOwnerManager())
        {
            if (!mgr->IsMouseDown())
            {
                const bool shouldCommit =
                    state.draggingLabel && state.draggingSlider == slider && *dragDidMove;
                ClearLabelDragState();
                if (shouldCommit)
                    slider->NotifyValueChanged();
                return;
            }
        }
        if (state.draggingLabel && state.draggingSlider == slider) {
            float deltaX = e.X - state.dragStartX;
            float range = slider->GetMax() - slider->GetMin();
            float sensitivity =
                (range > 0.0f) ? (range / kInspectorDragSliderPixelsPerFullRange) : 0.01f;
            float newValue = state.dragStartFloatValue + deltaX * sensitivity;
            newValue = std::max(slider->GetMin(), std::min(slider->GetMax(), newValue));
            slider->SetValueWithoutNotify(newValue);
            *dragDidMove = true;
            state.dragDidMove = true;
            slider->NotifyValueChanging();
            if (onChanging) onChanging();
            e.Stop();
        }
    });

    label->RegisterEventHandler(kEventMouseUp, [label, slider, onChanged, dragDidMove](UIEvent& e) {
        auto& state = GetDragState();
        if (state.draggingLabel && state.draggingSlider == slider && e.Button == 0) {
            SetLabelDragHighlight(label, false);
            state.draggingLabel = nullptr;
            state.draggingSlider = nullptr;
            state.dragDidMove = false;
            if (*dragDidMove) {
                slider->NotifyValueChanged();
                if (onChanged) onChanged();
            }
        }
    });
}

// ============================================================================
// Convenience helpers that create a complete row with label, field, and drag
// ============================================================================

// Create a row with a label and FloatField, with drag-to-adjust on the label.
// defaultValue controls what double-clicking the label resets to; defaults to initial.
template <typename ChangingFn, typename ChangedFn>
inline FloatField* AddFloatRowWithDrag(UIElement* parent, const std::string& labelText,
                                       float initial, ChangingFn&& onChanging, ChangedFn&& onChanged,
                                       float defaultValue = std::numeric_limits<float>::quiet_NaN(),
                                       const char* tooltip = nullptr,
                                       float minValue = -std::numeric_limits<float>::infinity(),
                                       float maxValue = std::numeric_limits<float>::infinity())
{
    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    FloatField* field = InspectorUI::AddFloat(fieldContainer, initial);
    // Copy before forwarding: SetupLabelDragFloat needs its own live copies.
    // std::forward would move the lambdas into the field, leaving the originals in a
    // moved-from state (e.g. null shared_ptr captures) for the drag callbacks.
    auto clampToRange = [field, minValue, maxValue](float value) {
        const float clamped = std::clamp(value, minValue, maxValue);
        if (clamped != value)
            field->SetValueWithoutNotify(clamped);
        return clamped;
    };
    std::function<void(float)> changingCopy = std::forward<ChangingFn>(onChanging);
    std::function<void(float)> changedCopy = std::forward<ChangedFn>(onChanged);
    auto changingField = changingCopy;
    auto changedField = changedCopy;
    field->SetOnValueChanging(
        [clampToRange, cb = std::move(changingField)](const float& value) mutable {
            if (cb)
                cb(clampToRange(value));
        });
    field->SetOnValueChanged(
        [clampToRange, cb = std::move(changedField)](const float& value) mutable {
            if (cb)
                cb(clampToRange(value));
        });
    const float resetTo = std::isnan(defaultValue) ? initial : defaultValue;
    SetupLabelDragFloat(label, field,
        [field, clampToRange, changingCopy = std::move(changingCopy)]() {
            if (changingCopy)
                changingCopy(clampToRange(field->GetValue()));
        },
        [field, clampToRange, changedCopy = std::move(changedCopy)]() {
            if (changedCopy)
                changedCopy(clampToRange(field->GetValue()));
        },
        resetTo,
        minValue,
        maxValue);
    return field;
}

// Create a row with a label and IntField, with drag-to-adjust on the label.
// defaultValue controls what double-clicking the label resets to; defaults to initial.
template <typename ChangingFn, typename ChangedFn>
inline IntField* AddIntRowWithDrag(UIElement* parent, const std::string& labelText,
                                   int initial, ChangingFn&& onChanging, ChangedFn&& onChanged,
                                   int defaultValue = std::numeric_limits<int>::min(),
                                   const char* tooltip = nullptr)
{
    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    IntField* field = InspectorUI::AddInt(fieldContainer, initial);
    auto changingCopy = onChanging;
    auto changedCopy  = onChanged;
    field->SetOnValueChanging(std::forward<ChangingFn>(onChanging));
    field->SetOnValueChanged(std::forward<ChangedFn>(onChanged));
    const int resetTo = (defaultValue == std::numeric_limits<int>::min()) ? initial : defaultValue;
    SetupLabelDragInt(label, field,
        [field, changingCopy = std::move(changingCopy)]() { changingCopy(field->GetValue()); },
        [field, changedCopy  = std::move(changedCopy)]()  { changedCopy(field->GetValue()); },
        resetTo);
    return field;
}

// Create a row with a label and Toggle (drag label right = on, left = off)
template <typename ChangedFn>
inline Toggle* AddToggleRow(UIElement* parent, const std::string& labelText,
                            bool initial, ChangedFn&& onChanged, const char* tooltip = nullptr)
{
    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->AddClass("inspector-field-toggle");  /* so CSS can align toggle left/center/right */
    Toggle* toggle = InspectorUI::AddToggle(fieldContainer, initial);
    toggle->SetOnValueChanged(std::forward<ChangedFn>(onChanged));
    SetupLabelDragToggle(label, toggle,
        [toggle, onChanged]() { onChanged(toggle->GetValue()); });
    return toggle;
}

// Create a row with a label and Slider. Returns both the Label and Slider so the
// caller can wire callbacks and SetupLabelDragSlider as needed.
inline std::pair<Label*, Slider*> AddSliderRow(UIElement* parent, const std::string& labelText,
                                                float initial, float min, float max,
                                                const char* tooltip = nullptr)
{
    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, labelText, tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

    auto slider = std::make_unique<Slider>();
    slider->AddClass("property-slider");
    ApplyInspectorSliderTrackInset(slider.get());
    slider->SetMin(min);
    slider->SetMax(max);
    slider->SetStep(0.0f);
    slider->SetValue(initial);
    slider->SetShowValueBubble(true);
    Slider* sliderRaw = slider.get();
    fieldContainer->AddChild(std::move(slider));
    return {label, sliderRaw};
}

inline void ApplySliderWithValueContainerStyle(
    UIElement* fieldContainer,
    float rightInsetPx = kInspectorSliderValueRightInsetPx)
{
    if (!fieldContainer)
        return;

    fieldContainer->AddClass("inspector-slider-with-value");
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(4.0f))
        .Set(Style::MarginRight, StyleLength::Px(rightInsetPx));
}

inline void ApplySliderFloatValueFieldStyle(
    FloatField* valueField,
    float widthPx = kInspectorSliderValueFieldWidthPx)
{
    if (!valueField)
        return;

    valueField->AddClass("inspector-float-field");
    valueField->AddClass("inspector-slider-value-field");
    valueField->Overrides()
        .Set(Style::Width, StyleLength::Px(widthPx))
        .Set(Style::MinWidth, StyleLength::Px(widthPx))
        .Set(Style::MaxWidth, StyleLength::Px(widthPx))
        .Set(Style::FlexShrink, 0.0f);
}

inline void ApplySliderIntValueFieldStyle(
    IntField* valueField,
    float widthPx = kInspectorSliderValueFieldWidthPx)
{
    if (!valueField)
        return;

    valueField->AddClass("inspector-int-field");
    valueField->AddClass("inspector-slider-value-field");
    valueField->Overrides()
        .Set(Style::Width, StyleLength::Px(widthPx))
        .Set(Style::MinWidth, StyleLength::Px(widthPx))
        .Set(Style::MaxWidth, StyleLength::Px(widthPx))
        .Set(Style::FlexShrink, 0.0f);
}

struct SliderWithFloatValueRow
{
    UIElement* Row = nullptr;
    Label* Label = nullptr;
    UIElement* FieldContainer = nullptr;
    Slider* Slider = nullptr;
    FloatField* ValueField = nullptr;
};

struct SliderWithIntValueRow
{
    UIElement* Row = nullptr;
    Label* Label = nullptr;
    UIElement* FieldContainer = nullptr;
    Slider* Slider = nullptr;
    IntField* ValueField = nullptr;
};

inline SliderWithFloatValueRow AddSliderWithFloatValueRow(
    UIElement* parent,
    const std::string& labelText,
    float initial,
    float min,
    float max,
    const char* tooltip = nullptr,
    bool showValueBubble = false,
    float rightInsetPx = kInspectorSliderValueRightInsetPx)
{
    SliderWithFloatValueRow result;
    result.Row = InspectorUI::AddRow(parent);
    result.Label = InspectorUI::AddLabel(result.Row, labelText, tooltip);
    result.FieldContainer = InspectorUI::AddFieldContainer(result.Row);
    ApplySliderWithValueContainerStyle(result.FieldContainer, rightInsetPx);

    result.Slider = AddInspectorSlider(result.FieldContainer, initial, min, max, 0.0f, showValueBubble,
                                       kInspectorSliderWithValueMinWidthPx);

    auto valueField = std::make_unique<FloatField>();
    valueField->SetValue(initial);
    ApplySliderFloatValueFieldStyle(valueField.get());
    result.ValueField = valueField.get();
    result.FieldContainer->AddChild(std::move(valueField));
    return result;
}

// Create a row with a label, a horizontal Slider (track, thumb, value bubble), the value in an
// editable field and, when `unit` is set, the unit after it, for a float with a known
// [min, max] range. The slider and the field stay in step: a drag fires onChanging (preview) and
// its release onChanged (commit) with the slider's value; a typed value is clamped to the range,
// moves the slider and fires onChanged once. A caller that sets the value from outside sets both
// the row's Slider and its ValueField.
template <typename ChangingFn, typename ChangedFn>
inline SliderWithFloatValueRow AddFloatSliderRow(UIElement* parent, const std::string& labelText,
                                                 float initial, float minValue, float maxValue,
                                                 ChangingFn&& onChanging, ChangedFn&& onChanged,
                                                 const char* tooltip = nullptr, const char* unit = nullptr)
{
    SliderWithFloatValueRow row =
        AddSliderWithFloatValueRow(parent, labelText, initial, minValue, maxValue, tooltip, /*showValueBubble=*/true);
    if (unit)
    {
        auto unitLabel = std::make_unique<Label>();
        unitLabel->AddClass("inspector-slider-unit");
        unitLabel->SetText(unit);
        row.FieldContainer->AddChild(std::move(unitLabel));
    }
    Slider* slider = row.Slider;
    FloatField* field = row.ValueField;
    std::function<void(float)> changed = std::forward<ChangedFn>(onChanged);
    slider->SetOnValueChanging([field, cb = std::forward<ChangingFn>(onChanging)](const float& value) {
        field->SetValueWithoutNotify(value);
        cb(value);
    });
    slider->SetOnValueChanged([field, changed](const float& value) {
        field->SetValueWithoutNotify(value);
        changed(value);
    });
    field->SetOnValueChanged([slider, field, minValue, maxValue, changed](const float& typed) {
        const float value = std::clamp(typed, minValue, maxValue);
        slider->SetValueWithoutNotify(value);
        field->SetValueWithoutNotify(value);
        changed(value);
    });
    return row;
}

inline SliderWithIntValueRow AddSliderWithIntValueRow(
    UIElement* parent,
    const std::string& labelText,
    int initial,
    int min,
    int max,
    const char* tooltip = nullptr,
    bool showValueBubble = false,
    float rightInsetPx = kInspectorSliderValueRightInsetPx)
{
    SliderWithIntValueRow result;
    result.Row = InspectorUI::AddRow(parent);
    result.Label = InspectorUI::AddLabel(result.Row, labelText, tooltip);
    result.FieldContainer = InspectorUI::AddFieldContainer(result.Row);
    ApplySliderWithValueContainerStyle(result.FieldContainer, rightInsetPx);

    result.Slider = AddInspectorSlider(
        result.FieldContainer,
        static_cast<float>(initial),
        static_cast<float>(min),
        static_cast<float>(max),
        1.0f,
        showValueBubble,
        kInspectorSliderWithValueMinWidthPx);

    auto valueField = std::make_unique<IntField>();
    valueField->SetValue(initial);
    ApplySliderIntValueFieldStyle(valueField.get());
    result.ValueField = valueField.get();
    result.FieldContainer->AddChild(std::move(valueField));
    return result;
}

// ============================================================================
// Auto-discovery: Automatically set up drag on all label+field pairs in a tree
// ============================================================================

// Find a child element by class name (first match, non-recursive for direct children)
inline UIElement* FindChildByClass(UIElement* parent, const std::string& className)
{
    if (!parent)
        return nullptr;
    for (const auto& child : parent->GetChildren()) {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

// Find a FloatField in an element's descendants
inline FloatField* FindFloatField(UIElement* element)
{
    if (!element)
        return nullptr;
    
    // Check if this element is a FloatField
    if (auto* ff = dynamic_cast<FloatField*>(element))
        return ff;
    
    // Search children
    for (const auto& child : element->GetChildren()) {
        if (auto* ff = FindFloatField(child.get()))
            return ff;
    }
    return nullptr;
}

// Find an IntField in an element's descendants
inline IntField* FindIntField(UIElement* element)
{
    if (!element)
        return nullptr;
    
    // Check if this element is an IntField
    if (auto* intF = dynamic_cast<IntField*>(element))
        return intF;
    
    // Search children
    for (const auto& child : element->GetChildren()) {
        if (auto* intF = FindIntField(child.get()))
            return intF;
    }
    return nullptr;
}

// Find a Toggle in an element's descendants
inline Toggle* FindToggle(UIElement* element)
{
    if (!element)
        return nullptr;
    if (auto* t = dynamic_cast<Toggle*>(element))
        return t;
    for (const auto& child : element->GetChildren()) {
        if (auto* t = FindToggle(child.get()))
            return t;
    }
    return nullptr;
}

// Find a Label in an element's descendants
inline Label* FindLabel(UIElement* element)
{
    if (!element)
        return nullptr;
    
    // Check if this element is a Label with inspector-label class
    if (auto* lbl = dynamic_cast<Label*>(element)) {
        if (lbl->HasClass("inspector-label") || lbl->HasClass("inspector-field-label"))
            return lbl;
    }
    
    // Search children
    for (const auto& child : element->GetChildren()) {
        if (auto* lbl = FindLabel(child.get()))
            return lbl;
    }
    return nullptr;
}

// Process a single row: find label and field, set up drag with auto-notification
inline void SetupDragForRow(UIElement* row)
{
    if (!row)
        return;
    
    Label* label = nullptr;
    FloatField* floatField = nullptr;
    IntField* intField = nullptr;
    Toggle* toggle = nullptr;
    
    for (const auto& child : row->GetChildren()) {
        if (!label)
            label = FindLabel(child.get());
        if (!floatField && !intField)
            floatField = FindFloatField(child.get());
        if (!floatField && !intField)
            intField = FindIntField(child.get());
        if (!toggle)
            toggle = FindToggle(child.get());
    }
    
    // Set up drag if we found a label and a numeric field or toggle
    if (label && label->HasClass("inspector-label-draggable"))
        return;
    if (label && floatField) {
        SetupLabelDragFloat(label, floatField,
            [floatField]() { floatField->NotifyValueChanging(); },
            [floatField]() { floatField->NotifyValueChanged(); },
            floatField->GetValue());  // double-click resets to current/default value
    } else if (label && intField) {
        SetupLabelDragInt(label, intField,
            [intField]() { intField->NotifyValueChanging(); },
            [intField]() { intField->NotifyValueChanged(); },
            intField->GetValue());  // double-click resets to current/default value
    } else if (label && toggle) {
        SetupLabelDragToggle(label, toggle,
            [toggle]() { toggle->NotifyValueChanged(); });
    }
}

// Automatically set up drag support for all label+field pairs in an inspector section
// Call this after building the inspector UI
inline void AutoSetupDragForInspector(UIElement* inspectorRoot)
{
    if (!inspectorRoot)
        return;
    
    // Find all rows with class "inspector-row"
    std::function<void(UIElement*)> processElement = [&](UIElement* element) {
        if (!element)
            return;
        
        if (element->HasClass("inspector-row")) {
            SetupDragForRow(element);
        }
        
        // Recurse into children
        for (const auto& child : element->GetChildren()) {
            processElement(child.get());
        }
    };
    
    processElement(inspectorRoot);
}

} // namespace InspectorDrag
} // namespace GameEngine

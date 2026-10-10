#include "UI/Controls/FloatField.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <cmath>

namespace GameEngine {
namespace {

// Same pixel-to-value scale as InspectorDrag::kInspectorDragFloatSensitivity
// so graph value boxes and inspector float labels feel identical.
constexpr float kDragToChangeSensitivity = 0.03f;
constexpr float kDragToChangeThresholdPx = 4.0f;
constexpr float kDragToChangeFineScale = 0.1f;

int DragModifierKeys(UIElement* element)
{
    if (UIManager* owner = element ? element->GetOwnerManager() : nullptr)
        return owner->GetModifierKeys();
    return 0;
}

} // namespace

void FloatField::EnableDragToChange()
{
    if (m_DragToChangeEnabled)
        return;

    m_DragToChangeEnabled = true;
    AddClass("float-field-drag-to-change");

    TextInput* editor = GetTextInput();
    if (!editor)
        return;

    editor->RegisterEventHandler(kEventMouseDown, [this, editor](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        // A disabled control never activates: no scrub, and no capture that
        // would swallow the press.
        if (!IsEnabled())
            return;
        BeginDragToChange(e.X);
        e.Capture(editor);
        e.Stop();
    });
    editor->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
    {
        if (!m_DragPending && !m_DraggingValue)
            return;
        UpdateDragToChange(e.X);
        e.Stop();
    });
    editor->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0 || (!m_DragPending && !m_DraggingValue))
            return;
        EndDragToChange();
        e.Stop();
    });
}

void FloatField::BeginDragToChange(float mouseX)
{
    m_SuppressTextFocus = false;
    m_FocusableBeforeDrag = IsFocusable();
    SetFocusable(false);
    if (UIManager* owner = GetOwnerManager())
        owner->SetFocusById({});
    FloatField::OnFocusChanged(false);
    m_DragPending = true;
    m_DraggingValue = false;
    m_DragStartX = mouseX;
    m_DragStartValue = GetValue();
}

void FloatField::UpdateDragToChange(float mouseX)
{
    if (!m_DragPending && !m_DraggingValue)
        return;

    const float deltaX = mouseX - m_DragStartX;
    if (!m_DraggingValue && std::fabs(deltaX) <= kDragToChangeThresholdPx)
        return;

    if (!m_DraggingValue)
        m_SuppressTextFocus = true;
    m_DraggingValue = true;

    const int mods = DragModifierKeys(this);
    float adjustedDeltaX = deltaX;
    if ((mods & Input::kModShift) != 0)
        adjustedDeltaX *= kDragToChangeFineScale;
    float newValue = m_DragStartValue + adjustedDeltaX * kDragToChangeSensitivity;
    if (Input::IsPrimaryShortcutModifier(mods))
        newValue = std::round(newValue);
    SetValue(ClampToValueRange(newValue));
    NotifyValueChanging();
}

void FloatField::EndDragToChange()
{
    const bool wasDragging = m_DraggingValue;
    m_DragPending = false;
    m_DraggingValue = false;
    if (wasDragging)
    {
        FloatField::OnFocusChanged(false);
        m_SuppressTextFocus = false;
        NotifyValueChanged();
        PostSafeAction([this, focusable = m_FocusableBeforeDrag]()
        {
            SetFocusable(focusable);
        });
        return;
    }

    SetFocusable(m_FocusableBeforeDrag);
    m_SuppressTextFocus = false;
    FloatField::OnFocusChanged(true);
    SelectAll();
}

bool FloatField::OnChar(unsigned int codepoint)
{
    if (m_DragToChangeEnabled && m_SuppressTextFocus)
        return false;
    return TextFieldBase<float>::OnChar(codepoint);
}

bool FloatField::OnKey(int key, int mods, UI::IPlatformApi* platform)
{
    if (m_DragToChangeEnabled && m_SuppressTextFocus)
        return false;
    return TextFieldBase<float>::OnKey(key, mods, platform);
}

void FloatField::OnPointerDown(float mouseX, float mouseY,
                              float x, float y, float w, float h,
                              const ResolvedStyle& style,
                              Rendering::Text::FontAtlas* font)
{
    if (m_DragToChangeEnabled)
    {
        BeginDragToChange(mouseX);
        return;
    }
    TextFieldBase<float>::OnPointerDown(mouseX, mouseY, x, y, w, h, style, font);
}

void FloatField::OnPointerDrag(float mouseX, float mouseY,
                              float x, float y, float w, float h,
                              const ResolvedStyle& style,
                              Rendering::Text::FontAtlas* font)
{
    if (m_DragToChangeEnabled)
    {
        UpdateDragToChange(mouseX);
        return;
    }
    TextFieldBase<float>::OnPointerDrag(mouseX, mouseY, x, y, w, h, style, font);
}

void FloatField::OnEvent(UIEvent& e)
{
    Field<float>::OnEvent(e);
    if (m_DragToChangeEnabled && e.Id == kEventMouseUp && (m_DragPending || m_DraggingValue))
    {
        EndDragToChange();
        e.Stop();
    }
}

} // namespace GameEngine

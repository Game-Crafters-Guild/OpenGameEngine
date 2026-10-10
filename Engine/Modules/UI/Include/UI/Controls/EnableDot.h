#pragma once

#include "UI/Controls/Toggle.h"

namespace GameEngine {

// An on/off switch drawn as a small dot: filled when on, a hollow ring when off, a dash when the
// values it stands for disagree (SetMixed). The element is the hit area and a child mark is the
// visual; the mark carries the state class the control's sheet draws (enable-dot-mark-on,
// enable-dot-mark-off, enable-dot-mark-mixed), so no rule reaches the mark through its parent's
// state. Mouse, keyboard, mixed and pressed behaviour are ToggleBase's.
class EnableDot : public ToggleBase
{
public:
    EnableDot();

    // The value stays readable but muted: the switch keeps its own state while something above it
    // is off (a component on an entity that is switched off).
    void SetInactive(bool inactive);
    bool IsInactive() const { return m_Inactive; }

    void OnEvent(UIEvent& e) override;

protected:
    void OnToggleStateChanged() override;

private:
    UIElement* m_Mark = nullptr;
    bool m_Inactive = false;
};

} // namespace GameEngine

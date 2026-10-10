#pragma once

#include <string>

#include "UI/Controls/Toggle.h"

namespace GameEngine {

class Label;
class UIElement;

// Checkbox control built on ToggleBase. Shares toggle behavior with Toggle
// but presents a checkbox-style visual using a child Label for text.
class Checkbox : public ToggleBase
{
public:
    Checkbox();

    void SetText(const std::string& text);
    const std::string& GetText() const { return m_Text; }

private:
        void EnsureBox();
    void EnsureLabel();

        UIElement*  m_BoxChild{nullptr};
    Label*      m_LabelChild{nullptr};
    std::string m_Text;
};

} // namespace GameEngine


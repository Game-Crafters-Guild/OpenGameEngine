#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{
class Button;
class Label;

namespace EditorUI
{

// A notice in an inspector: one statement the user has to read before trusting the fields around
// it, and optionally the one action that resolves it (discard a preserved value, select the entity
// that generates this one).
//
// The plate, the text and the action are declared in
// UI/controls/InspectorNotice/InspectorNotice.uxml and styled by InspectorNotice.css beside it. The
// layout is instantiated in the constructor, not when the notice gains a UIManager, so the text and
// the action are in the element tree from the frame the notice is built: a tree scan, a search or a
// test reads them without a layout pass. The action starts hidden; SetAction shows it.
//
// A warning (the default) says something is wrong or about to be lost and is drawn in the warning
// palette; an information notice states how the fields around it behave and is drawn neutral. The
// kind is one class on the control (inspector-notice-warning, inspector-notice-information) that
// InspectorNotice.css styles.
class InspectorNotice : public UIElement
{
public:
    enum class Kind
    {
        Warning,
        Information,
    };

    explicit InspectorNotice(const std::string& text, Kind kind = Kind::Warning);

    // Shows the action button with `label` and `tooltip`; a click calls `onInvoke`.
    void SetAction(const std::string& label, const std::string& tooltip, std::function<void()> onInvoke);

    // Opens the notice with a heading: the kind's glyph and `title`, above the text.
    void SetTitle(const std::string& title);

    // Replaces the notice's text.
    void SetText(const std::string& text);

private:
    void InvokeAction();

    // Owned children, built in the constructor from the layout and never replaced. Null only when
    // the layout did not load, which is logged.
    Label* m_Text = nullptr;
    Button* m_Action = nullptr;
    UIElement* m_Heading = nullptr;
    Label* m_Title = nullptr;
    std::function<void()> m_OnInvoke;
};

} // namespace EditorUI
} // namespace GameEngine

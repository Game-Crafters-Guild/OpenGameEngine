#pragma once

#include "UI/Controls/TextArea.h"

#include <functional>

namespace GameEngine
{

struct UIEvent;

/// The AI Assistant's input: a TextArea whose key-down events pass through a filter
/// first, so the panel can take Esc (stop) and Up/Down (recall)
/// before the text area edits with them.
class PromptTextArea final : public TextArea
{
public:
    /// Returns true when it handled the key; the text area then ignores it.
    using KeyFilter = std::function<bool(UIEvent&)>;

    void SetKeyFilter(KeyFilter filter);

    void OnEvent(UIEvent& event) override;

private:
    KeyFilter m_KeyFilter;
};

} // namespace GameEngine

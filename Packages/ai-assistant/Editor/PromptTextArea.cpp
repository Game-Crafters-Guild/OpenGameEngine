#include "PromptTextArea.h"

#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine
{

void PromptTextArea::SetKeyFilter(KeyFilter filter)
{
    m_KeyFilter = std::move(filter);
}

void PromptTextArea::OnEvent(UIEvent& event)
{
    if (event.Id == kEventKeyDown && m_KeyFilter && m_KeyFilter(event))
        return;
    TextArea::OnEvent(event);
}

} // namespace GameEngine

#include "UI/Controls/BaseField.h"

#include "Logger/Logger.h"

namespace GameEngine {
// BaseField provides default no-op handlers; the only out-of-line thing it needs is the
// diagnostic below, which lives here so the logger stays out of a header every control includes.

namespace UI::Detail
{
void ReportReentrantValueWrite(const UIElement& element)
{
    // Rate limiting is the CALLER's, and it is per element (Field<T>::m_ReentrantWriteReported):
    // a handler that writes on every change would otherwise flood the ring it shares with
    // everything else, and a process-wide latch would let the first offender hide every other.
    const std::string& id = element.GetId();
    Logger::Log::Warning(
        "[UI Field] A value handler on element id='{}' wrote back to the field it was handling. "
        "The write was applied WITHOUT notifying, because notifying from inside a notification is "
        "unbounded recursion. Say so directly with SetValueWithoutNotify, or defer the write with "
        "PostAction if it should be announced as its own change.",
        id.empty() ? "<no-id>" : id.c_str());
}
} // namespace UI::Detail

}

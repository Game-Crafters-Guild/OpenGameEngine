#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace GameEngine
{
/// One server-sent event: the `event:` field and the joined `data:` lines.
struct SseEvent
{
    /// The event's type; "message" when the stream named none.
    std::string Type;
    /// The event's data lines joined with '\n'.
    std::string Data;
};

/// Frames a text/event-stream body into events. Bytes may arrive split anywhere,
/// a line break included: the parser keeps the unfinished line and event between
/// Feed() calls. Lines end in LF, CRLF or CR; a blank line ends an event; a line
/// starting with ':' is a comment; fields other than `event` and `data` are ignored.
class SseParser
{
public:
    using EventCallback = std::function<void(const SseEvent& event)>;

    /// Consumes the next bytes of the stream and calls `onEvent` for every event
    /// they complete, in order.
    void Feed(std::string_view bytes, const EventCallback& onEvent);

private:
    void ProcessLine(std::string_view line, const EventCallback& onEvent);

    std::string m_Line;
    std::string m_EventType;
    std::string m_Data;
    bool m_HasData = false;
    // The previous byte was a CR, so an LF that follows it ends no second line.
    bool m_SkipLineFeed = false;
};
} // namespace GameEngine

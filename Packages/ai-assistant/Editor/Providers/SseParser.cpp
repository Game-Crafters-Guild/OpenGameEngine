#include "SseParser.h"

namespace GameEngine
{
void SseParser::Feed(std::string_view bytes, const EventCallback& onEvent)
{
    for (const char byte : bytes)
    {
        if (m_SkipLineFeed)
        {
            m_SkipLineFeed = false;
            if (byte == '\n')
                continue;
        }
        if (byte == '\r' || byte == '\n')
        {
            m_SkipLineFeed = byte == '\r';
            ProcessLine(m_Line, onEvent);
            m_Line.clear();
            continue;
        }
        m_Line.push_back(byte);
    }
}

void SseParser::ProcessLine(std::string_view line, const EventCallback& onEvent)
{
    if (line.empty())
    {
        if (m_HasData)
            onEvent(SseEvent{m_EventType.empty() ? "message" : m_EventType, m_Data});
        m_EventType.clear();
        m_Data.clear();
        m_HasData = false;
        return;
    }
    if (line.front() == ':')
        return;

    const size_t colon = line.find(':');
    const std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);

    if (field == "event")
    {
        m_EventType = value;
    }
    else if (field == "data")
    {
        if (m_HasData)
            m_Data.push_back('\n');
        m_Data.append(value);
        m_HasData = true;
    }
}
} // namespace GameEngine

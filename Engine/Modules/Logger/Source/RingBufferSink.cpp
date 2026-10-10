#include "Logger/RingBufferSink.h"
#include <algorithm>

namespace Logger {

RingBufferSink::RingBufferSink(size_t capacity)
    : m_Capacity(capacity < 1 ? 1 : capacity)
{
    m_Buffer.resize(m_Capacity);
}

void RingBufferSink::Write(const LogMessage& message)
{
    std::lock_guard lock(m_Mutex);

    auto& slot = m_Buffer[m_Head];
    slot.Level = message.Level;
    slot.Message = message.Message;
    slot.Timestamp = message.Timestamp;

    m_Head = (m_Head + 1) % m_Capacity;
    if (m_Count < m_Capacity)
    {
        ++m_Count;
    }
}

std::vector<RingBufferSink::StoredMessage> RingBufferSink::GetMessages(
    LogLevel minLevel,
    size_t maxCount,
    const String& substring,
    size_t offset) const
{
    std::lock_guard lock(m_Mutex);

    std::vector<StoredMessage> result;
    result.reserve(maxCount > 0 ? std::min(maxCount, m_Count) : m_Count);

    // Newest entry is at (m_Head - 1); walk backwards toward the oldest so that
    // truncating to maxCount keeps the most recent matches (the tail of the log).
    // offset skips that many of the newest matches before collecting.
    for (size_t i = 0; i < m_Count; ++i)
    {
        const auto& entry = m_Buffer[(m_Head + m_Capacity - 1 - i) % m_Capacity];

        if (entry.Level < minLevel)
        {
            continue;
        }

        if (!substring.empty() && entry.Message.find(substring) == String::npos)
        {
            continue;
        }

        if (offset > 0)
        {
            --offset;
            continue;
        }

        result.push_back(entry);

        if (maxCount > 0 && result.size() >= maxCount)
        {
            break;
        }
    }

    // Collected newest-first; reverse so callers see oldest-first within the window.
    std::reverse(result.begin(), result.end());
    return result;
}

size_t RingBufferSink::GetCount() const
{
    std::lock_guard lock(m_Mutex);
    return m_Count;
}

void RingBufferSink::Clear()
{
    std::lock_guard lock(m_Mutex);
    m_Head = 0;
    m_Count = 0;
}

} // namespace Logger

#include "Logger/DeferredSink.h"

#include <cassert>
#include <format>

namespace Logger
{

DeferredSink::DeferredSink(size_t heldRecordLimit)
    : m_HeldRecordLimit(heldRecordLimit)
{
}

void DeferredSink::Attach(UniquePtr<LogSink> sink)
{
    assert(sink && "DeferredSink::Attach needs the sink the held records go to");
    std::lock_guard<std::mutex> lock(m_Mutex);
    assert(!m_Target && "DeferredSink::Attach is called once; a second sink would take no held records");
    if (m_Target || !sink)
        return;
    m_Target = std::move(sink);
    for (const LogMessage& message : m_Held)
        WriteToTarget(message);
    // Error, so a target that filters by level still learns its log has a gap.
    if (m_DroppedRecordCount > 0)
    {
        LogMessage notice(LogLevel::Error,
                          std::format("Logger: {} records logged before this log opened were dropped "
                                      "after the first {} were held",
                                      m_DroppedRecordCount, m_HeldRecordLimit));
        WriteToTarget(notice);
    }
    m_Held.clear();
    m_Held.shrink_to_fit();
    m_DroppedRecordCount = 0;
}

void DeferredSink::Write(const LogMessage& message)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Target)
    {
        WriteToTarget(message);
        return;
    }
    if (m_Held.size() < m_HeldRecordLimit)
        m_Held.push_back(message);
    else
        ++m_DroppedRecordCount;
}

void DeferredSink::Flush()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Target)
        m_Target->Flush();
}

void DeferredSink::WriteToTarget(const LogMessage& message)
{
    if (m_Target->IsAvailable() && m_Target->ShouldLog(message.Level))
        m_Target->Write(message);
}

} // namespace Logger

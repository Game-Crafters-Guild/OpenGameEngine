#pragma once

#include "Logger/LogSink.h"
#include "Logger/Types.h"
#include <mutex>
#include <vector>

namespace Logger {

// Thread-safe circular buffer sink that retains the last N log messages
// for programmatic access by debug tools (e.g. EditorDebugServer).
class RingBufferSink : public LogSink
{
public:
    struct StoredMessage
    {
        LogLevel Level = LogLevel::Info;
        String Message;
        TimePoint Timestamp;
    };

    explicit RingBufferSink(size_t capacity = 500);
    ~RingBufferSink() override = default;

    void Write(const LogMessage& message) override;
    void Flush() override {}
    bool ShouldLog(LogLevel /*level*/) const override { return true; }
    String GetName() const override { return "RingBuffer"; }

    // Get a snapshot of stored messages, oldest first within the returned window.
    // When maxCount truncates, the *most recent* maxCount matches are returned
    // (the tail of the log), so a small count shows what just happened.
    // Optional filters: minLevel, maxCount (0 = unlimited), substring match.
    // offset skips the newest matching messages before collecting (paging from
    // the tail backwards; 0 = start at the newest).
    std::vector<StoredMessage> GetMessages(
        LogLevel minLevel = LogLevel::Trace,
        size_t maxCount = 0,
        const String& substring = "",
        size_t offset = 0) const;

    size_t GetCount() const;

    void Clear();

private:
    mutable std::mutex m_Mutex;
    std::vector<StoredMessage> m_Buffer;
    size_t m_Capacity;
    size_t m_Head = 0;
    size_t m_Count = 0;
};

} // namespace Logger

#pragma once

#include "Logger/LogSink.h"
#include "Logger/Types.h"
#include <mutex>

namespace Logger
{

/**
 * @brief Sink that holds records until the sink that should receive them exists.
 *
 * For a host whose log destination depends on something it must first read, and
 * whose reading can itself log: the Player's log file sits in a directory named
 * by the game configuration, and a configuration that fails to parse says so
 * before that directory is known. Register a DeferredSink at startup, then hand
 * it the real sink with Attach. The held records are written to that sink first,
 * in the order they were logged, and every later record goes straight through.
 *
 * Holding is bounded so a host that never attaches cannot grow without limit.
 * The earliest records are kept, since they carry the cause; the sink reports
 * how many it dropped when it attaches.
 */
class DeferredSink : public LogSink
{
  public:
    static constexpr size_t kDefaultHeldRecordLimit = 1024;

    explicit DeferredSink(size_t heldRecordLimit = kDefaultHeldRecordLimit);
    ~DeferredSink() override = default;

    /// Writes the held records to sink (those its ShouldLog accepts), then
    /// forwards every later record to it. Call once, with a sink; development
    /// builds assert on a second call or a null sink.
    void Attach(UniquePtr<LogSink> sink);

    void Write(const LogMessage& message) override;
    void Flush() override;
    // Holding takes every level; the attached sink filters when it receives.
    bool ShouldLog(LogLevel /*level*/) const override { return true; }
    String GetName() const override { return "Deferred"; }

  private:
    void WriteToTarget(const LogMessage& message);

    mutable std::mutex m_Mutex;
    UniquePtr<LogSink> m_Target;
    Vector<LogMessage> m_Held;
    size_t m_HeldRecordLimit;
    size_t m_DroppedRecordCount = 0;
};

} // namespace Logger

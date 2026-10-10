#pragma once

#include "Logger/LogSink.h"
#include "Logger/Types.h"
#include <functional>
#include <mutex>
#include <vector>

namespace Logger
{

/**
 * @brief Callback-based log sink that forwards messages to registered listeners.
 *
 * This sink allows UI components (like LogView) to receive log messages in real-time.
 * It's thread-safe and can handle callbacks from multiple threads.
 */
class CallbackSink : public LogSink
{
  public:
    using LogCallback = std::function<void(const LogMessage&)>;

    struct Config
    {
        LogLevel MinLevel = LogLevel::Trace;
    };

    CallbackSink();
    explicit CallbackSink(const Config& config);
    ~CallbackSink() override = default;

    void Write(const LogMessage& message) override;
    void Flush() override {}
    bool ShouldLog(LogLevel level) const override;
    String GetName() const override { return "Callback"; }
    bool IsAvailable() const override { return true; }

    /**
     * @brief Register a callback to receive log messages.
     * @param callback Function to call when a log message is received.
     * @return A unique ID that can be used to unregister the callback.
     */
    uint64 RegisterCallback(LogCallback callback);

    /**
     * @brief Unregister a callback by its ID.
     * @param id The ID returned by RegisterCallback.
     */
    void UnregisterCallback(uint64 id);

    /**
     * @brief Set the minimum log level for this sink.
     * @param level Minimum level to forward to callbacks.
     */
    void SetMinLevel(LogLevel level) { m_Config.MinLevel = level; }

    /**
     * @brief Get the current minimum log level.
     * @return Current minimum log level.
     */
    LogLevel GetMinLevel() const { return m_Config.MinLevel; }

  private:
    Config m_Config;

    struct CallbackEntry
    {
        uint64 id;
        LogCallback callback;
    };

    std::vector<CallbackEntry> m_Callbacks;
    mutable std::mutex m_CallbackMutex;
    uint64 m_NextCallbackId = 1;
};

} // namespace Logger

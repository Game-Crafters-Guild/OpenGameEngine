#pragma once

// Capture of the engine's log stream, for tests that assert on what a code path
// reports.
//
// Engine is a SHARED library and Logger a STATIC one: engine code logs through
// Engine.dll's Logger state, not this executable's copy. The capture adopts the
// engine's state (RedirectToSharedState) or its sink sees nothing — and a capture
// that sees nothing is indistinguishable from a notice that was never emitted, so
// a test proves the sink live with one line of its own before reading a zero out
// of it.
//
// RAII: the sink holds a raw pointer to the test's vector, and a fatal assertion
// would skip a trailing ClearSinks() — the next engine log line would then write
// through a dangling pointer and take the whole suite down. The destructor clears
// on every exit path, ASSERT aborts included, and then puts a working logger back.
// The logger state is process-global: a scope that ends with the engine unable to
// log has silenced every test that follows it in the binary, errors included, while
// the suite still reports green.

#include "Core/EngineLoggerBridge.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::TestLog
{

class CapturingLogSink final : public Logger::LogSink
{
  public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Out->push_back(std::string(message.Message));
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "TestCapturingLogSink"; }

  private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};

// Routes every engine log line at or above minLevel into *out for the scope's
// lifetime. Call Logger::Log::Flush() before reading: dispatch is asynchronous.
class ScopedEngineLogCapture
{
  public:
    explicit ScopedEngineLogCapture(std::vector<std::string>* out,
                                    Logger::LogLevel minLevel = Logger::LogLevel::Info)
    {
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        m_LevelOnEntry = Logger::Log::GetLogLevel();
        Logger::Log::Initialize({minLevel, false});
        Logger::Log::ClearSinks();
        Logger::Log::AddSink(std::make_unique<CapturingLogSink>(out));
    }

    ~ScopedEngineLogCapture()
    {
        // Dispatch is asynchronous: a line logged during this capture's life is delivered to it,
        // not to whatever sink a caller installs next.
        Logger::Log::Flush();
        Logger::Log::ClearSinks();
        Logger::Log::Initialize({RestoreLevel(), false});
    }

    ScopedEngineLogCapture(const ScopedEngineLogCapture&) = delete;
    ScopedEngineLogCapture& operator=(const ScopedEngineLogCapture&) = delete;

  private:
    // The console sink Initialize() installs keeps its own copy of the minimum level and
    // is only installed when no sink is left, so coming back at Off would pin a sink that
    // drops everything and that no later Initialize can replace — worse than the state
    // being repaired. A process that had no level of its own comes back at Info, which is
    // what the shipping hosts configure when nothing else has.
    Logger::LogLevel RestoreLevel() const
    {
        return m_LevelOnEntry == Logger::LogLevel::Off ? Logger::LogLevel::Info : m_LevelOnEntry;
    }

    Logger::LogLevel m_LevelOnEntry = Logger::LogLevel::Off;
};

inline size_t CountLinesContaining(const std::vector<std::string>& lines, std::string_view needle)
{
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(),
        [needle](const std::string& l) { return l.find(needle) != std::string::npos; }));
}

inline std::string FirstLineContaining(const std::vector<std::string>& lines, std::string_view needle)
{
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos)
            return l;
    return {};
}

} // namespace GameEngine::TestLog

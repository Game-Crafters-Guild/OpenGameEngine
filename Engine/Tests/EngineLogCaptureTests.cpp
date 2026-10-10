// The logger state a capture scope borrows is process-global, so what the scope gives
// back is every later test's starting condition.

#include <gtest/gtest.h>

#include "Core/EngineLoggerBridge.h"
#include "EngineLogCapture.h"
#include "Logger/LogLevel.h"
#include "Logger/Logger.h"
#include "ScopedStdStreamCapture.h"

#include <string>
#include <vector>

using GameEngine::TestLog::CountLinesContaining;
using GameEngine::TestLog::ScopedEngineLogCapture;
using GameEngine::TestLog::ScopedStdStreamCapture;

namespace
{
constexpr const char* kInsideLine = "EngineLogCaptureTests: inside the scope";
constexpr const char* kAfterLine = "EngineLogCaptureTests: an error after the scope";
} // namespace

// Clearing the sinks is half of what the scope does to the process: Logger::ClearSinks
// also drops the effective level to Off, and Logger::Initialize installs a console sink
// only when no sink is left. A scope that ended on the clear therefore left the binary
// unable to log at all, and every error after it vanished while the suite still exited 0.
TEST(ScopedEngineLogCaptureLifetime, LeavesTheEngineAbleToLogAfterTheScope)
{
    // Engine is a shared library and Logger a static one: read and write the state the
    // engine logs through, not this executable's copy.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());

    // The shape of a fresh test binary: nothing configured.
    Logger::Log::ClearSinks();
    ASSERT_EQ(0u, Logger::Log::GetSinkCount());

    std::vector<std::string> captured;
    {
        ScopedEngineLogCapture capture(&captured, Logger::LogLevel::Info);
        Logger::Log::Info(kInsideLine);
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(captured, kInsideLine)) << "the capture never worked to begin with";

    EXPECT_GT(Logger::Log::GetSinkCount(), 0u) << "the scope left the process with no sink";
    EXPECT_LE(Logger::Log::GetLogLevel(), Logger::LogLevel::Error)
        << "the scope left a level that drops errors";

    // The two above are state; this is the consequence. An error logged after the scope
    // has to actually arrive somewhere.
    std::string streamOutput;
    {
        ScopedStdStreamCapture streams;
        Logger::Log::Error(kAfterLine);
        Logger::Log::Flush();
        streamOutput = streams.Text();
    }
    EXPECT_NE(std::string::npos, streamOutput.find(kAfterLine))
        << "an error logged after the capture scope reached no sink:\n"
        << streamOutput;
}

// Restoring is not the same as configuring: a binary that had chosen a level keeps it,
// so a capture in the middle of a fixture does not quietly make the rest of it noisier.
TEST(ScopedEngineLogCaptureLifetime, RestoresTheLevelTheProcessHadOnEntry)
{
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::ClearSinks();
    Logger::Log::Initialize({Logger::LogLevel::Warning, false});
    ASSERT_EQ(Logger::LogLevel::Warning, Logger::Log::GetLogLevel());

    std::vector<std::string> captured;
    {
        ScopedEngineLogCapture capture(&captured, Logger::LogLevel::Debug);
        Logger::Log::Debug(kInsideLine);
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(captured, kInsideLine)) << "the capture never worked to begin with";

    EXPECT_EQ(Logger::LogLevel::Warning, Logger::Log::GetLogLevel())
        << "the scope handed back a level the process did not ask for";
}

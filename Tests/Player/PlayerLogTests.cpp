// The Player's log file opens only once the game configuration is read, in the
// per-user directory of the game it names, and every running copy of a game
// writes its own file there.

#include "PlayerLog.h"

#include "EngineLogCapture.h"
#include "Engine/Build/GameConfig.h"
#include "Logger/DeferredSink.h"
#include "Logger/Logger.h"
#include "Platform/Process.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if LOGGER_ENABLE_FILE_LOGGING

namespace fs = std::filesystem;

namespace
{

std::string ReadWholeFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteWholeFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

fs::path ThisProcessLogFile(const fs::path& logsDirectory)
{
    return logsDirectory / ("game-" + std::to_string(GameEngine::Platform::GetCurrentProcessId()) + ".log");
}

} // namespace

// The Player has no console, so a configuration error that never reaches the file
// is an error nobody can see, and the game silently runs on defaults.
TEST(PlayerLog, AMalformedConfigurationReachesTheLogFile)
{
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_player_log_config"));
    const fs::path config = temp.Path() / "malformed.config";
    WriteWholeFile(config, "{ \"gameName\": \"Probe Game\", this is not json");
    const fs::path logsDirectory = temp.Path() / "Logs";

    std::vector<std::string> captured;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&captured);
        Logger::DeferredSink& heldRecords = GameEngine::StartPlayerLog();
        (void)GameEngine::LoadGameConfig(config);
        // Dispatched, so the error is held rather than still queued when the file opens.
        Logger::Log::Flush();
        GameEngine::OpenPlayerLogFile(heldRecords, logsDirectory);
        Logger::Log::Flush();
    }
    ASSERT_EQ(GameEngine::TestLog::CountLinesContaining(captured, "GameConfig: Parse error"), 1u)
        << "the configuration loader never reported the error at all";

    const std::string log = ReadWholeFile(ThisProcessLogFile(logsDirectory));
    EXPECT_NE(log.find("GameConfig: Parse error"), std::string::npos) << log;
    EXPECT_NE(log.find("malformed.config"), std::string::npos) << log;
    EXPECT_NE(log.find("Player: log file"), std::string::npos) << log;
}

// Two copies of one game share a logs directory. Opening this process's log must
// leave a file another copy is writing alone.
TEST(PlayerLog, OpeningTheLogLeavesAnotherProcessesLogIntact)
{
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_player_log_two"));
    const fs::path logsDirectory = temp.Path() / "Logs";
    const std::vector<fs::path> otherLogs = {
        logsDirectory / "game.log",
        logsDirectory / ("game-" + std::to_string(GameEngine::Platform::GetCurrentProcessId() + 1) + ".log"),
    };
    for (const fs::path& other : otherLogs)
        WriteWholeFile(other, "another running copy\n");

    std::vector<std::string> captured;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&captured);
        Logger::DeferredSink& heldRecords = GameEngine::StartPlayerLog();
        GameEngine::OpenPlayerLogFile(heldRecords, logsDirectory);
        Logger::Log::Flush();
    }

    for (const fs::path& other : otherLogs)
        EXPECT_EQ(ReadWholeFile(other), "another running copy\n") << other;
    EXPECT_NE(ReadWholeFile(ThisProcessLogFile(logsDirectory)).find("Player: log file"), std::string::npos);
}

// A read-only profile leaves the logs directory uncreatable. The Player must say
// it has no log file rather than announce one that does not exist.
TEST(PlayerLog, ALogFileThatCannotOpenIsReportedAsMissing)
{
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_player_log_blocked"));
    // A file where the logs directory's parent should be: the directory cannot be created.
    const fs::path blocker = temp.Path() / "blocker";
    WriteWholeFile(blocker, "not a directory");
    const fs::path logsDirectory = blocker / "Logs";

    std::vector<std::string> captured;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&captured);
        Logger::DeferredSink& heldRecords = GameEngine::StartPlayerLog();
        GameEngine::OpenPlayerLogFile(heldRecords, logsDirectory);
        Logger::Log::Flush();
    }

    EXPECT_FALSE(fs::exists(ThisProcessLogFile(logsDirectory)));
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(captured, "Player: log file '"), 0u);
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(captured, "Player: cannot open the log file"), 1u);
}

#endif // LOGGER_ENABLE_FILE_LOGGING

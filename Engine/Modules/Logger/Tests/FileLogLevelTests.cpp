// What reaches the log file versus what reaches the rest of the sinks.
//
// The file is the artifact read after a run, so it records at Info even while
// the global level sits at Debug for the console and for the in-memory ring that
// debug tools read. GE_LOG_FILE_LEVEL moves the file, and nothing else.

#include "Logger/FileSink.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include <gtest/gtest.h>

#include "Logger/Logger.h"
#include "Logger/RingBufferSink.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

using namespace Logger;

constexpr const char* kDebugProbe = "file-log-level-probe-debug";
constexpr const char* kInfoProbe = "file-log-level-probe-info";

void SetFileLogLevelEnv(const char* value)
{
#ifdef _WIN32
    // An empty value removes the variable on Windows.
    _putenv_s(kFileLogLevelEnvVar, value != nullptr ? value : "");
#else
    if (value != nullptr)
        setenv(kFileLogLevelEnvVar, value, 1);
    else
        unsetenv(kFileLogLevelEnvVar);
#endif
}

// The Editor's arrangement: global level Debug, a ring sink that takes
// everything the global level admits, and one file sink at the resolved file
// level.
class FileLogLevelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        SetFileLogLevelEnv(nullptr);

        m_Path = std::filesystem::temp_directory_path() /
                 ("ge_file_log_level_" +
                  std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".log");
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);

        Log::Config cfg;
        cfg.GlobalMinLevel = LogLevel::Debug;
        Log::Initialize(cfg);
        Log::ClearSinks();

        auto ring = std::make_unique<RingBufferSink>(64);
        m_Ring = ring.get();
        Log::AddSink(std::move(ring));
    }

    void TearDown() override
    {
        // Sinks first: the file handle has to be released before the file goes.
        Log::ClearSinks();
        m_Ring = nullptr;
        Log::Shutdown();
        SetFileLogLevelEnv(nullptr);
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    void AttachFileSinkAtResolvedLevel()
    {
        FileSink::Config cfg;
        cfg.filename = m_Path.string();
        cfg.append = false;
        cfg.minLevel = ResolveFileLogLevel().level;
        Log::AddSink(std::make_unique<FileSink>(cfg));
    }

    std::string LogProbesAndReadFile()
    {
        Log::Debug(kDebugProbe);
        Log::Info(kInfoProbe);
        Log::Flush();

        std::ifstream in(m_Path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path m_Path;
    RingBufferSink* m_Ring = nullptr;
};

TEST_F(FileLogLevelTest, DefaultKeepsDebugOutOfTheFile)
{
    ASSERT_EQ(ResolveFileLogLevel().level, LogLevel::Info);
    AttachFileSinkAtResolvedLevel();

    const std::string content = LogProbesAndReadFile();

    EXPECT_EQ(content.find(kDebugProbe), std::string::npos) << "Debug reached the file at the default level";
    EXPECT_NE(content.find(kInfoProbe), std::string::npos);
}

TEST_F(FileLogLevelTest, DefaultLeavesTheRingAtTheGlobalLevel)
{
    AttachFileSinkAtResolvedLevel();

    LogProbesAndReadFile();

    // The file sink's level is its own; the ring keeps every record the global
    // level admits, which is what live log queries read.
    EXPECT_EQ(m_Ring->GetMessages(LogLevel::Debug).size(), 2u);
    EXPECT_EQ(Log::GetLogLevel(), LogLevel::Debug);
}

TEST_F(FileLogLevelTest, EnvironmentOverrideRestoresDebugInTheFile)
{
    SetFileLogLevelEnv("debug");
    ASSERT_EQ(ResolveFileLogLevel().level, LogLevel::Debug);
    AttachFileSinkAtResolvedLevel();

    const std::string content = LogProbesAndReadFile();

    EXPECT_NE(content.find(kDebugProbe), std::string::npos)
        << "GE_LOG_FILE_LEVEL=debug did not widen the file";
    EXPECT_NE(content.find(kInfoProbe), std::string::npos);
}

TEST_F(FileLogLevelTest, EnvironmentOverrideNarrowsBelowInfo)
{
    SetFileLogLevelEnv("warn");
    ASSERT_EQ(ResolveFileLogLevel().level, LogLevel::Warning);
    AttachFileSinkAtResolvedLevel();

    const std::string content = LogProbesAndReadFile();

    EXPECT_EQ(content.find(kDebugProbe), std::string::npos);
    EXPECT_EQ(content.find(kInfoProbe), std::string::npos);
}

TEST_F(FileLogLevelTest, ARequestBelowTheGlobalLevelResolvesToTheGlobalLevel)
{
    SetFileLogLevelEnv("trace");

    // Log::Trace never enqueues a record while the global level is Debug, so no
    // sink can record one. The resolved level reports that rather than promising
    // a level the file cannot deliver.
    EXPECT_EQ(ResolveFileLogLevel().level, LogLevel::Debug);
}

TEST_F(FileLogLevelTest, TheGlobalLevelBoundsWhatTheFileCanRecord)
{
    Log::SetLogLevel(LogLevel::Info);
    SetFileLogLevelEnv("debug");
    ASSERT_EQ(ResolveFileLogLevel().level, LogLevel::Info);
    AttachFileSinkAtResolvedLevel();

    const std::string content = LogProbesAndReadFile();

    EXPECT_EQ(content.find(kDebugProbe), std::string::npos)
        << "a record the global level dropped reached the file";
    EXPECT_NE(content.find(kInfoProbe), std::string::npos);
}

TEST_F(FileLogLevelTest, TheNoSinkGateValueBoundsNothing)
{
    // Log::GetLogLevel() reads the call-site gate, which sits at Off while the
    // logger holds no sink. A host that resolves before it attaches one must not
    // come away with a file sink that records nothing.
    Log::ClearSinks();
    ASSERT_EQ(Log::GetLogLevel(), LogLevel::Off);

    SetFileLogLevelEnv("debug");
    EXPECT_EQ(ResolveFileLogLevel().level, LogLevel::Debug);
}

TEST_F(FileLogLevelTest, TheEnvironmentValueIsReadInAnyCapitalization)
{
    for (const char* spelling : {"debug", "DEBUG", "Debug", "DeBuG"})
    {
        SetFileLogLevelEnv(spelling);
        const FileLogLevel resolved = ResolveFileLogLevel();

        EXPECT_EQ(resolved.level, LogLevel::Debug) << spelling;
        EXPECT_TRUE(resolved.unrecognizedValue.empty()) << spelling;
    }
}

TEST_F(FileLogLevelTest, AskingForInfoIsNotATypo)
{
    // Info is also what a value naming no level falls back to, so it is the one
    // spelling a request and a typo cannot be told apart by level alone.
    for (const char* spelling : {"info", "INFO", "Info"})
    {
        SetFileLogLevelEnv(spelling);
        const FileLogLevel resolved = ResolveFileLogLevel();

        EXPECT_EQ(resolved.level, LogLevel::Info) << spelling;
        EXPECT_TRUE(resolved.unrecognizedValue.empty()) << spelling;
    }
}

TEST_F(FileLogLevelTest, UnrecognizedEnvironmentValueResolvesToInfo)
{
    SetFileLogLevelEnv("verbose");
    const FileLogLevel resolved = ResolveFileLogLevel();

    EXPECT_EQ(resolved.level, LogLevel::Info);
    // Handed back so the host can name it: a fallback nobody can see reads as
    // "the variable did nothing".
    EXPECT_EQ(resolved.unrecognizedValue, "verbose");
}

} // namespace

#endif // LOGGER_ENABLE_FILE_LOGGING

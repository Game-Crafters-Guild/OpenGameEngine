// What FileSink does when it cannot open its file at all.
//
// Write() reopens lazily whenever the file is closed, and OpenFile() creates the
// parent directory, opens, and reports to stderr. A path that cannot be opened —
// a read-only directory, a full disk — therefore costs that whole sequence on
// every log line. The Editor now attaches a file sink on every host by default,
// so this is reachable by ordinary misconfiguration rather than only by an
// explicit -logfile.
//
// The failure is provoked with a directory sitting where the log file goes:
// opening a directory for writing fails on every platform.

#include "Logger/FileSink.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace Logger;

namespace
{

LogMessage MakeMessage(int ordinal)
{
    LogMessage message(LogLevel::Info, "open-probe-" + std::to_string(ordinal));
    message.SetThreadId("test");
    return message;
}

class FileSinkOpenFailureTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() /
                ("ge_filesink_open_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
        ASSERT_TRUE(std::filesystem::create_directories(m_Dir, ec)) << ec.message();
        m_LogPath = m_Dir / "Test.log";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    FileSink::Config ConfigForLogPath() const
    {
        FileSink::Config cfg;
        cfg.filename = m_LogPath.string().c_str();
        cfg.append = true;
        cfg.autoFlush = true;
        return cfg;
    }

    std::filesystem::path m_Dir;
    std::filesystem::path m_LogPath;
};

// The probe has to be able to report the other state: with an openable path the
// same sequence does produce a file.
TEST_F(FileSinkOpenFailureTest, AnOpenableSinkWritesItsFile)
{
    FileSink sink(ConfigForLogPath());
    sink.Write(MakeMessage(0));
    sink.Flush();

    EXPECT_TRUE(std::filesystem::is_regular_file(m_LogPath));
}

// The first failed open is the answer for the whole session. Removing the
// obstruction afterwards makes a retry observable: if Write() still reopened, a
// file would appear where the directory used to be.
TEST_F(FileSinkOpenFailureTest, AFailedOpenIsNotRetriedOnEveryWrite)
{
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(m_LogPath, ec)) << ec.message();

    FileSink sink(ConfigForLogPath());

    ASSERT_TRUE(std::filesystem::remove(m_LogPath, ec)) << ec.message();
    ASSERT_FALSE(std::filesystem::exists(m_LogPath));

    for (int i = 0; i < 50; ++i)
    {
        sink.Write(MakeMessage(i));
    }
    sink.Flush();

    EXPECT_FALSE(std::filesystem::exists(m_LogPath));
}

// The directory the sink would have created is the other half of the per-write
// cost, and it must not be recreated either.
TEST_F(FileSinkOpenFailureTest, AFailedOpenStopsRecreatingTheParentDirectory)
{
    const std::filesystem::path nested = m_Dir / "nested";
    const std::filesystem::path nestedLog = nested / "Test.log";

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(nestedLog, ec)) << ec.message();

    FileSink::Config cfg;
    cfg.filename = nestedLog.string().c_str();
    cfg.append = true;
    cfg.autoFlush = true;
    FileSink sink(cfg);

    ASSERT_TRUE(std::filesystem::remove_all(nested, ec) > 0) << ec.message();
    ASSERT_FALSE(std::filesystem::exists(nested));

    for (int i = 0; i < 50; ++i)
    {
        sink.Write(MakeMessage(i));
    }
    sink.Flush();

    EXPECT_FALSE(std::filesystem::exists(nested));
}

} // namespace

#endif // LOGGER_ENABLE_FILE_LOGGING

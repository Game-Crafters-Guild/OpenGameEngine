// FileSink rotation behaviour, in particular what happens when the rotation
// cannot be performed.
//
// Rotation is a rename. On Windows the rename is denied while any other process
// holds the log open, which is the normal state when two editors share a log
// path. The rename's error code used to be discarded, and because OpenFile
// re-stats the file size straight back to the cap, every subsequent write then
// closed, failed to rename, and reopened — forever.
//
// The failure is provoked here with a non-empty directory sitting where the
// backup would go: renaming onto that fails on every platform, so the test does
// not depend on Windows sharing semantics to reach the path under test.

#include "Logger/FileSink.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace Logger;

namespace
{

std::string ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A line long enough that a handful of them crosses the small caps used here.
LogMessage MakeMessage(int ordinal)
{
    LogMessage message(LogLevel::Info, "rotation-probe-" + std::to_string(ordinal) + "-" + std::string(64, 'x'));
    message.SetThreadId("test");
    return message;
}

class FileSinkRotationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() /
                ("ge_filesink_rotation_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
        ASSERT_TRUE(std::filesystem::create_directories(m_Dir, ec)) << ec.message();
        m_LogPath = m_Dir / "Test.log";
        m_BackupPath = m_Dir / "Test.1.log";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    FileSink::Config SmallRotatingConfig() const
    {
        FileSink::Config cfg;
        cfg.filename = m_LogPath.string().c_str();
        cfg.append = true;
        cfg.autoFlush = true;
        cfg.maxFileSize = 256;
        // Exactly one backup, so RotateFile's shift loop does not run. With more
        // than one it would rename the blocker below out of the way (src .1 -> .2
        // succeeds for a directory) and the rotation under test would succeed.
        cfg.maxBackupFiles = 1;
        return cfg;
    }

    // Make the rotation rename fail: renaming a file onto an existing NON-EMPTY
    // directory is an error on every platform we build for, and RotateFile's
    // "delete the oldest backup" step cannot clear it either (fs::remove only
    // removes empty directories). Deterministic, unlike relying on Windows
    // sharing semantics from within one process.
    void BlockTheBackupPath() const
    {
        std::error_code ec;
        ASSERT_TRUE(std::filesystem::create_directories(m_BackupPath, ec)) << ec.message();
        std::ofstream occupant(m_BackupPath / "occupant.txt");
        ASSERT_TRUE(occupant.is_open());
        occupant << "this directory is not renameable-onto";
    }

    // Verify the instrument: the blocker only proves anything while it is still an
    // un-renameable-onto directory. A first draft of this fixture used two backups
    // and the shift loop quietly moved the blocker aside, so the rotation being
    // tested actually succeeded.
    void ExpectRotationWasActuallyBlocked() const
    {
        ASSERT_TRUE(std::filesystem::is_directory(m_BackupPath))
            << "the blocker is gone, so the rename under test was not blocked and this "
               "test proves nothing";
        ASSERT_TRUE(std::filesystem::exists(m_BackupPath / "occupant.txt"))
            << "the blocker must still be non-empty for the rename to keep failing";
    }

    std::filesystem::path m_Dir;
    std::filesystem::path m_LogPath;
    std::filesystem::path m_BackupPath;
};

} // namespace

// Control: with nothing in the way, rotation still happens. Without this, the
// blocked test below could pass because rotation never triggered at all.
TEST_F(FileSinkRotationTest, RotatesNormallyWhenTheBackupPathIsFree)
{
    {
        FileSink sink(SmallRotatingConfig());
        for (int i = 0; i < 40; ++i)
        {
            sink.Write(MakeMessage(i));
        }
    }

    EXPECT_TRUE(std::filesystem::exists(m_BackupPath))
        << "the cap should have been crossed and a backup produced";
    EXPECT_TRUE(std::filesystem::is_regular_file(m_BackupPath));
}

// A guard, not a regression test: the old code also kept writing here (it appended
// after each failed rotation), so this passes with and without the fix. It is worth
// keeping because "stop retrying" must not be implemented by giving up on the file.
// The behaviour that actually changed is covered by RotationStaysDisabledAfterItFails.
TEST_F(FileSinkRotationTest, AFailedRotationKeepsWriting)
{
    BlockTheBackupPath();

    {
        FileSink sink(SmallRotatingConfig());
        for (int i = 0; i < 40; ++i)
        {
            sink.Write(MakeMessage(i));
        }
    }

    ExpectRotationWasActuallyBlocked();

    const std::string content = ReadWholeFile(m_LogPath);
    // Lines from before the cap and from well after it must both be present: the
    // sink kept writing through the failed rotation rather than thrashing or
    // truncating.
    EXPECT_NE(content.find("rotation-probe-0-"), std::string::npos) << "early lines were lost";
    EXPECT_NE(content.find("rotation-probe-39-"), std::string::npos) << "the sink stopped writing";
    EXPECT_GT(content.size(), 256u) << "the file should have grown past the cap it could not rotate at";
}

// Once rotation has failed it stays off, so a sink under a long-lived blocker does
// not close/reopen the file on every single write. Observable as: freeing the
// blocker afterwards does not resurrect rotation for this sink.
TEST_F(FileSinkRotationTest, RotationStaysDisabledAfterItFails)
{
    BlockTheBackupPath();

    FileSink sink(SmallRotatingConfig());
    for (int i = 0; i < 40; ++i)
    {
        sink.Write(MakeMessage(i));
    }
    ExpectRotationWasActuallyBlocked();

    // Remove the blocker. A sink that kept retrying every write would rotate now.
    std::error_code ec;
    std::filesystem::remove_all(m_BackupPath, ec);
    ASSERT_FALSE(std::filesystem::exists(m_BackupPath));

    for (int i = 40; i < 80; ++i)
    {
        sink.Write(MakeMessage(i));
    }

    EXPECT_FALSE(std::filesystem::exists(m_BackupPath))
        << "rotation was disabled after its failure and must not silently resume";
    EXPECT_NE(ReadWholeFile(m_LogPath).find("rotation-probe-79-"), std::string::npos)
        << "the sink must still be writing";
}

// Config::append governs only the first open. A reopen mid-session — which is what
// a rotation performs — must never truncate the session's own log.
TEST_F(FileSinkRotationTest, ANonAppendingSinkDoesNotEraseItsOwnLogOnReopen)
{
    BlockTheBackupPath();

    FileSink::Config cfg = SmallRotatingConfig();
    cfg.append = false; // start clean, then keep everything this session writes

    FileSink sink(cfg);
    for (int i = 0; i < 40; ++i)
    {
        sink.Write(MakeMessage(i));
    }
    ExpectRotationWasActuallyBlocked();

    const std::string content = ReadWholeFile(m_LogPath);
    EXPECT_NE(content.find("rotation-probe-0-"), std::string::npos)
        << "the failed rotation's reopen truncated the log it had just failed to rotate";
    EXPECT_NE(content.find("rotation-probe-39-"), std::string::npos);
}

#endif // LOGGER_ENABLE_FILE_LOGGING

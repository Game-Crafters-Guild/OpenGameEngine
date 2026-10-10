// What FileSink puts on each line: severity, timestamp and source location.
//
// LogSink::FormatMessage takes (message, includeTimestamp, includeLevel,
// includeSource) and every flag has a plausible default, so a caller that omits
// one argument still compiles — the remaining arguments just shift into the
// wrong parameters and the defaults quietly fill the tail. Nothing about that is
// visible at the call site, so each flag is pinned here to a difference in the
// file itself.

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

// A directory-qualified path, so the assertions also cover FormatMessage
// reducing it to the bare filename.
constexpr const char* kSourceFile = "C:\\probe\\dir\\FormatProbe.cpp";
constexpr const char* kSourceFunction = "ProbeFunction";
constexpr int kSourceLine = 4242;
constexpr const char* kSourceStamp = "[FormatProbe.cpp:4242 in ProbeFunction]";
constexpr const char* kProbeText = "format-probe";

class FileSinkFormatTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() /
                ("ge_filesink_format_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
        ASSERT_TRUE(std::filesystem::create_directories(m_Dir, ec)) << ec.message();
        m_LogPath = m_Dir / "Format.log";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    FileSink::Config BaseConfig() const
    {
        FileSink::Config cfg;
        cfg.filename = m_LogPath.string().c_str();
        cfg.append = false;
        cfg.autoFlush = true;
        cfg.maxFileSize = 0; // Rotation off: this fixture is about line content.
        return cfg;
    }

    // One line per severity, each carrying a source location. The sink is
    // destroyed before the file is read so the last write is on disk.
    std::string WriteAndRead(const FileSink::Config& cfg) const
    {
        {
            FileSink sink(cfg);
            for (LogLevel level : {LogLevel::Info, LogLevel::Warning, LogLevel::Error})
            {
                LogMessage message(level, kProbeText);
                message.SetThreadId("test");
                message.SourceFile = kSourceFile;
                message.SourceLine = kSourceLine;
                message.Function = kSourceFunction;
                sink.Write(message);
            }
        }
        return ReadWholeFile(m_LogPath);
    }

    std::filesystem::path m_Dir;
    std::filesystem::path m_LogPath;
};

} // namespace

// The severity is unconditional — FileSink::Config deliberately has no switch
// for it, because a log file whose lines carry no severity cannot be triaged.
// It is asserted with source location OFF because that is the configuration a
// shifted argument list silently disarms: passing includeSource where
// includeLevel belongs turns the severity off along with it.
TEST_F(FileSinkFormatTest, WritesTheLevelWhenSourceLocationIsDisabled)
{
    FileSink::Config cfg = BaseConfig();
    cfg.includeSource = false;

    const std::string content = WriteAndRead(cfg);

    ASSERT_NE(content.find(kProbeText), std::string::npos) << "nothing reached the file";
    EXPECT_NE(content.find("[INFO]"), std::string::npos) << "Info lines lost their severity";
    EXPECT_NE(content.find("[WARN]"), std::string::npos) << "Warning lines lost their severity";
    EXPECT_NE(content.find("[ERROR]"), std::string::npos) << "Error lines lost their severity";
}

TEST_F(FileSinkFormatTest, WritesTheLevelWhenSourceLocationIsEnabled)
{
    FileSink::Config cfg = BaseConfig();
    cfg.includeSource = true;

    const std::string content = WriteAndRead(cfg);

    ASSERT_NE(content.find(kProbeText), std::string::npos) << "nothing reached the file";
    EXPECT_NE(content.find("[INFO]"), std::string::npos);
    EXPECT_NE(content.find("[WARN]"), std::string::npos);
    EXPECT_NE(content.find("[ERROR]"), std::string::npos);
}

#if LOGGER_ENABLE_SOURCE_LOCATION

// includeSource has to reach FormatMessage's fourth parameter. Landing anywhere
// else leaves the fourth on its default (false), and the file then carries no
// source location however the sink is configured.
TEST_F(FileSinkFormatTest, WritesSourceLocationWhenConfigured)
{
    FileSink::Config cfg = BaseConfig();
    cfg.includeSource = true;

    const std::string content = WriteAndRead(cfg);

    ASSERT_NE(content.find(kProbeText), std::string::npos) << "nothing reached the file";
    EXPECT_NE(content.find(kSourceStamp), std::string::npos)
        << "includeSource=true did not produce a source location; content: " << content;
}

// Control for the test above: without it, a sink that stamped the source
// location unconditionally would also pass.
TEST_F(FileSinkFormatTest, OmitsSourceLocationWhenNotConfigured)
{
    FileSink::Config cfg = BaseConfig();
    cfg.includeSource = false;

    const std::string content = WriteAndRead(cfg);

    ASSERT_NE(content.find(kProbeText), std::string::npos) << "nothing reached the file";
    EXPECT_EQ(content.find("FormatProbe.cpp"), std::string::npos)
        << "includeSource=false still stamped a source location";
}

#endif // LOGGER_ENABLE_SOURCE_LOCATION

#if LOGGER_ENABLE_TIMESTAMPS

// The timestamp is the first segment, so with it off a line starts at the
// severity. This pins includeTimestamp to the first parameter the same way the
// tests above pin the other two.
TEST_F(FileSinkFormatTest, TimestampLeadsTheLineOnlyWhenConfigured)
{
    FileSink::Config withTimestamp = BaseConfig();
    withTimestamp.includeTimestamp = true;
    withTimestamp.includeSource = false;
    const std::string stamped = WriteAndRead(withTimestamp);

    FileSink::Config withoutTimestamp = BaseConfig();
    withoutTimestamp.includeTimestamp = false;
    withoutTimestamp.includeSource = false;
    const std::string bare = WriteAndRead(withoutTimestamp);

    ASSERT_NE(stamped.find(kProbeText), std::string::npos) << "nothing reached the file";
    ASSERT_NE(bare.find(kProbeText), std::string::npos) << "nothing reached the file";

    EXPECT_EQ(bare.rfind("[INFO] ", 0), 0u) << "an untimestamped line must start at the severity";
    EXPECT_NE(stamped.rfind("[INFO] ", 0), 0u) << "a timestamped line must not start at the severity";
    EXPECT_GT(stamped.size(), bare.size()) << "the timestamp adds nothing to the line";
}

#endif // LOGGER_ENABLE_TIMESTAMPS

#endif // LOGGER_ENABLE_FILE_LOGGING

#include <gtest/gtest.h>

#include "Panels/OnDiskFileName.h"

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>

using GameEngine::Editor::OnDiskFileName;

namespace
{

namespace fs = std::filesystem;

class OnDiskFileNameTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = GameEngine::TestUtils::MakeUniqueTempDirectory("ondiskname");
        fs::create_directories(m_Dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    void Write(const std::string& name)
    {
        std::ofstream out(m_Dir / name, std::ios::binary | std::ios::trunc);
        out << "// test\n";
    }

    fs::path m_Dir;
};

TEST_F(OnDiskFileNameTests, AnExactlyCasedPathIsReturnedUnchanged)
{
    Write("Rock.glsl");
    EXPECT_EQ(OnDiskFileName(m_Dir / "Rock.glsl"), "Rock.glsl");
}

// The regression: a registry-resolved path arrives case-folded on a case-insensitive
// filesystem, and titling a tab from it verbatim disagrees with the asset browser's
// on-disk-cased path for the same file.
TEST_F(OnDiskFileNameTests, ACaseFoldedPathRecoversTheOnDiskCase)
{
    Write("Rock.glsl");
    EXPECT_EQ(OnDiskFileName(m_Dir / "rock.glsl"), "Rock.glsl");
}

TEST_F(OnDiskFileNameTests, BothEntryPointProvenancesAgreeOnOneTitle)
{
    Write("Rock.glsl");
    EXPECT_EQ(OnDiskFileName(m_Dir / "Rock.glsl"), OnDiskFileName(m_Dir / "rock.glsl"));
}

TEST_F(OnDiskFileNameTests, AMissingFileFallsBackToTheGivenName)
{
    EXPECT_EQ(OnDiskFileName(m_Dir / "Absent.glsl"), "Absent.glsl");
}

TEST_F(OnDiskFileNameTests, AMissingDirectoryFallsBackRatherThanThrowing)
{
    EXPECT_EQ(OnDiskFileName(m_Dir / "no-such-dir" / "Rock.glsl"), "Rock.glsl");
}

TEST_F(OnDiskFileNameTests, AnEmptyPathIsHandled)
{
    EXPECT_EQ(OnDiskFileName(fs::path()), "");
}

} // namespace

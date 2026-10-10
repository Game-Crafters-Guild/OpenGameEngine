// CopyDirectoryKeepingLinks: the copy a build falls back to when it cannot
// move its staging directory into place. A Linux runtime ships versioned
// shared-library links and a macOS bundle ships framework links; the copy
// must reproduce each link as a link with its stored target.
//
// The link cases need permission to create symbolic links. Windows grants it
// only in Developer Mode or to an elevated process; without it those cases
// skip, and they run on Linux and macOS.

#include "Engine/Build/DirectoryCopy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace
{

void WriteText(const fs::path& path, const char* text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

class BuildDirectoryCopy : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("ge-directory-copy-" + std::to_string(id));
        m_From = m_Root / "from";
        m_To = m_Root / "to";
        fs::create_directories(m_From);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    // Creates `link` -> `target`, or reports that this process may not.
    bool MakeLink(const fs::path& target, const fs::path& link, bool directory)
    {
        std::error_code ec;
        if (directory)
            fs::create_directory_symlink(target, link, ec);
        else
            fs::create_symlink(target, link, ec);
        return !ec;
    }

    fs::path m_Root;
    fs::path m_From;
    fs::path m_To;
};

constexpr const char* kNoLinkPermission =
    "this process cannot create symbolic links (Windows needs Developer Mode or elevation)";

} // namespace

TEST_F(BuildDirectoryCopy, CopiesFilesAndNestedDirectories)
{
    WriteText(m_From / "Player", "player");
    WriteText(m_From / "Assets" / "Scenes" / "Main.scene", "scene");
    fs::create_directories(m_From / "Empty");

    std::error_code ec;
    EXPECT_TRUE(GameEngine::CopyDirectoryKeepingLinks(m_From, m_To, ec)) << ec.message();
    EXPECT_EQ(ReadText(m_To / "Player"), "player");
    EXPECT_EQ(ReadText(m_To / "Assets" / "Scenes" / "Main.scene"), "scene");
    EXPECT_TRUE(fs::is_directory(m_To / "Empty"));
}

TEST_F(BuildDirectoryCopy, FrameworkLayoutKeepsItsLinks)
{
    const fs::path framework = m_From / "Foo.framework";
    WriteText(framework / "Versions" / "A" / "Foo", "binary");
    if (!MakeLink("A", framework / "Versions" / "Current", true) ||
        !MakeLink(fs::path("Versions") / "Current" / "Foo", framework / "Foo", false))
        GTEST_SKIP() << kNoLinkPermission;

    std::error_code ec;
    ASSERT_TRUE(GameEngine::CopyDirectoryKeepingLinks(m_From, m_To, ec)) << ec.message();
    const fs::path copied = m_To / "Foo.framework";
    ASSERT_TRUE(fs::is_symlink(copied / "Versions" / "Current"));
    EXPECT_EQ(fs::read_symlink(copied / "Versions" / "Current"), fs::path("A"));
    ASSERT_TRUE(fs::is_symlink(copied / "Foo"));
    EXPECT_EQ(fs::read_symlink(copied / "Foo"), fs::path("Versions") / "Current" / "Foo");
    EXPECT_FALSE(fs::is_symlink(copied / "Versions" / "A" / "Foo"));
    EXPECT_EQ(ReadText(copied / "Foo"), "binary");
}

TEST_F(BuildDirectoryCopy, VersionedSharedLibraryChainKeepsItsLinks)
{
    WriteText(m_From / "libfoo.so.1.2", "library");
    if (!MakeLink("libfoo.so.1.2", m_From / "libfoo.so.1", false) ||
        !MakeLink("libfoo.so.1", m_From / "libfoo.so", false))
        GTEST_SKIP() << kNoLinkPermission;

    std::error_code ec;
    ASSERT_TRUE(GameEngine::CopyDirectoryKeepingLinks(m_From, m_To, ec)) << ec.message();
    ASSERT_TRUE(fs::is_symlink(m_To / "libfoo.so"));
    EXPECT_EQ(fs::read_symlink(m_To / "libfoo.so"), fs::path("libfoo.so.1"));
    ASSERT_TRUE(fs::is_symlink(m_To / "libfoo.so.1"));
    EXPECT_EQ(fs::read_symlink(m_To / "libfoo.so.1"), fs::path("libfoo.so.1.2"));
    EXPECT_EQ(ReadText(m_To / "libfoo.so"), "library");
}

TEST_F(BuildDirectoryCopy, LinkOutOfTheTreeIsCopiedNotFollowed)
{
    const fs::path outside = m_Root / "outside";
    WriteText(outside / "data", "outside");
    if (!MakeLink(fs::absolute(outside), m_From / "external", true))
        GTEST_SKIP() << kNoLinkPermission;

    std::error_code ec;
    ASSERT_TRUE(GameEngine::CopyDirectoryKeepingLinks(m_From, m_To, ec)) << ec.message();
    ASSERT_TRUE(fs::is_symlink(m_To / "external"));
    EXPECT_EQ(fs::read_symlink(m_To / "external"), fs::absolute(outside));
    // Nothing was written through the link.
    EXPECT_EQ(std::distance(fs::directory_iterator(outside), fs::directory_iterator{}), 1);
}

TEST_F(BuildDirectoryCopy, CancelStopsWithoutAnError)
{
    WriteText(m_From / "Player", "player");

    std::error_code ec;
    EXPECT_FALSE(GameEngine::CopyDirectoryKeepingLinks(m_From, m_To, ec, []() { return true; }));
    EXPECT_FALSE(ec);
    EXPECT_FALSE(fs::exists(m_To / "Player"));
}

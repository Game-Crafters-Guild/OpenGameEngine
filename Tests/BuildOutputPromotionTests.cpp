// PromoteBuildOutput: the last build step replaces the previous game with the
// completed staging directory. A failed promotion must leave the previous game
// and the candidate where they were, and a successful one must leave no backup.

#include "Engine/Build/OutputPromotion.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#if defined(__linux__)
#  include <sys/stat.h>
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{

#if defined(_WIN32)
// An open handle without FILE_SHARE_DELETE, the way a virus scanner or the
// search indexer opens a freshly written file. While it is open, Windows
// refuses to rename the directory that contains the file.
class HeldFile
{
  public:
    HeldFile(const fs::path& path, DWORD shareMode)
        : m_Handle(CreateFileW(path.c_str(), GENERIC_READ, shareMode, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~HeldFile() { Release(); }
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;

    bool IsHeld() const { return m_Handle != INVALID_HANDLE_VALUE; }
    void Release()
    {
        if (IsHeld())
            CloseHandle(m_Handle);
        m_Handle = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE m_Handle;
};

constexpr DWORD kShareReadWrite = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr DWORD kShareNothing = 0;
#endif

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

class BuildOutputPromotion : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("ge-promotion-" + std::to_string(id));
        m_Stage = m_Root / "candidate";
        m_Output = m_Root / "game";
        m_Backup = m_Root / "game.previous";
        m_Discard = m_Root / "game.discard";
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    bool Promote(const fs::path& stage, const fs::path& output,
                 const std::function<bool()>& shouldCancel = {})
    {
        return GameEngine::PromoteBuildOutput(stage, output, m_Errors, m_Warnings, shouldCancel);
    }

    fs::path m_Root;
    fs::path m_Stage;
    fs::path m_Output;
    fs::path m_Backup;
    fs::path m_Discard;
    std::vector<std::string> m_Errors;
    std::vector<std::string> m_Warnings;
};

} // namespace

TEST_F(BuildOutputPromotion, MissingCandidateFailsAndKeepsPreviousGame)
{
    WriteText(m_Output / "Player", "previous");

    EXPECT_FALSE(Promote(m_Stage, m_Output));
    EXPECT_FALSE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "previous");
    EXPECT_FALSE(fs::exists(m_Backup));
}

TEST_F(BuildOutputPromotion, LeftoverBackupBlocksPromotionAndChangesNothing)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    WriteText(m_Backup / "Player", "recovery");

    EXPECT_FALSE(Promote(m_Stage, m_Output));
    EXPECT_FALSE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "previous");
    EXPECT_EQ(ReadText(m_Stage / "Player"), "replacement");
    EXPECT_EQ(ReadText(m_Backup / "Player"), "recovery");
}

TEST_F(BuildOutputPromotion, ReplacementMovesCandidateAndRemovesBackup)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");

    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_TRUE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Stage));
    EXPECT_FALSE(fs::exists(m_Backup));
}

TEST_F(BuildOutputPromotion, LeftoverDiscardIsClearedAndDoesNotBlock)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    WriteText(m_Discard / "Engine.dll", "left by an earlier build");

    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_TRUE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Discard));
    EXPECT_FALSE(fs::exists(m_Backup));
}

#if defined(__linux__)
// A move between volumes can never succeed, so the promotion copies at once
// instead of waiting out the retries meant for a file another process holds.
TEST_F(BuildOutputPromotion, MoveToAnotherVolumeCopiesWithoutRetrying)
{
    const fs::path otherVolume = "/dev/shm";
    struct stat tempInfo{};
    struct stat otherInfo{};
    if (stat(m_Root.parent_path().c_str(), &tempInfo) != 0 || stat(otherVolume.c_str(), &otherInfo) != 0 ||
        tempInfo.st_dev == otherInfo.st_dev)
        GTEST_SKIP() << "needs /dev/shm on a different device from the temporary directory";

    const fs::path output = otherVolume / m_Root.filename();
    WriteText(m_Stage / "Player", "replacement");
    const auto start = std::chrono::steady_clock::now();
    const bool promoted = Promote(m_Stage, output);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const std::string copied = ReadText(output / "Player");
    std::error_code ec;
    fs::remove_all(output, ec);

    EXPECT_TRUE(promoted);
    EXPECT_EQ(copied, "replacement");
    EXPECT_LT(elapsed, std::chrono::milliseconds(150));
}
#endif

TEST_F(BuildOutputPromotion, FirstMacBundlePromotionKeepsBundleContents)
{
    const fs::path bundle = m_Root / "mac-stage" / "Game.app";
    const fs::path finalBundle = m_Root / "mac-output" / "Game.app";
    WriteText(bundle / "Contents" / "MacOS" / "Player", "Mac player");
    fs::create_directories(finalBundle.parent_path());

    EXPECT_TRUE(Promote(bundle, finalBundle));
    EXPECT_EQ(ReadText(finalBundle / "Contents" / "MacOS" / "Player"), "Mac player");
}

TEST_F(BuildOutputPromotion, CancelAfterBackupRestoresPreviousGame)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    const fs::path backup = m_Backup;

    EXPECT_FALSE(Promote(m_Stage, m_Output, [backup]() { return fs::exists(backup); }));
    ASSERT_EQ(m_Errors.size(), 1u);
    EXPECT_EQ(m_Errors[0], "Build cancelled; the previous output was kept");
    EXPECT_EQ(ReadText(m_Output / "Player"), "previous");
    EXPECT_EQ(ReadText(m_Stage / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Backup));
}

#if defined(_WIN32)
TEST_F(BuildOutputPromotion, HeldCandidateFileIsCopiedIntoPlace)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    HeldFile held(m_Stage / "Player", kShareReadWrite);
    ASSERT_TRUE(held.IsHeld());

    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_TRUE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Backup));
    // The held file keeps the staging directory alive; that is reported, not fatal.
    EXPECT_FALSE(m_Warnings.empty());
}

TEST_F(BuildOutputPromotion, UnreadableCandidateRestoresPreviousGame)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    HeldFile held(m_Stage / "Player", kShareNothing);
    ASSERT_TRUE(held.IsHeld());

    EXPECT_FALSE(Promote(m_Stage, m_Output));
    held.Release();
    ASSERT_EQ(m_Errors.size(), 1u);
    // System messages end in a period; inside a sentence it must not show.
    EXPECT_EQ(m_Errors[0].find(".;"), std::string::npos) << m_Errors[0];
    EXPECT_EQ(ReadText(m_Output / "Player"), "previous");
    EXPECT_EQ(ReadText(m_Stage / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Backup));
}

TEST_F(BuildOutputPromotion, HeldPreviousGameFailsWithoutChangingAnything)
{
    WriteText(m_Output / "Player", "previous");
    WriteText(m_Stage / "Player", "replacement");
    HeldFile held(m_Output / "Player", kShareReadWrite);
    ASSERT_TRUE(held.IsHeld());

    EXPECT_FALSE(Promote(m_Stage, m_Output));
    held.Release();
    ASSERT_EQ(m_Errors.size(), 1u);
    // The error says what to do, not only what the system reported.
    EXPECT_NE(m_Errors[0].find("Close the running game"), std::string::npos) << m_Errors[0];
    EXPECT_EQ(m_Errors[0].find(".)"), std::string::npos) << m_Errors[0];
    EXPECT_EQ(ReadText(m_Output / "Player"), "previous");
    EXPECT_EQ(ReadText(m_Stage / "Player"), "replacement");
    EXPECT_FALSE(fs::exists(m_Backup));
}
#endif

#if defined(_WIN32)
// A library loaded from the previous game (a running copy of it, or any
// loader) cannot be deleted. The build that replaces that game succeeds with
// a warning, and the builds after it are not blocked by what was left.
TEST_F(BuildOutputPromotion, LoadedLibraryInReplacedGameDoesNotBlockLaterBuilds)
{
    wchar_t exePath[MAX_PATH];
    ASSERT_NE(GetModuleFileNameW(nullptr, exePath, MAX_PATH), 0u);
    const fs::path library = fs::path(exePath).parent_path() / "gtest.dll";
    ASSERT_TRUE(fs::exists(library));

    WriteText(m_Output / "Player", "build 0");
    fs::copy_file(library, m_Output / "Game.dll");
    HMODULE loaded = LoadLibraryExW((m_Output / "Game.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    ASSERT_NE(loaded, nullptr);

    WriteText(m_Stage / "Player", "build 1");
    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_FALSE(m_Warnings.empty());
    EXPECT_FALSE(fs::exists(m_Backup));

    m_Errors.clear();
    WriteText(m_Stage / "Player", "build 2");
    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_TRUE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "build 2");

    FreeLibrary(loaded);
    m_Errors.clear();
    WriteText(m_Stage / "Player", "build 3");
    EXPECT_TRUE(Promote(m_Stage, m_Output));
    EXPECT_TRUE(m_Errors.empty());
    EXPECT_EQ(ReadText(m_Output / "Player"), "build 3");
    EXPECT_FALSE(fs::exists(m_Discard));
    EXPECT_FALSE(fs::exists(m_Backup));
}
#endif

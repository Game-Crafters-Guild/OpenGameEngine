#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include "FileWatcher/FileWatcher.h"
#include "TestTempDir.h"

namespace GameEngine
{

class FileWatcherExclusionTest : public ::testing::Test
{
  protected:
    FileWatcher watcher;

    // ShouldExcludeDirectory absolutizes its argument before expressing it
    // relative to the watch base, so the base must be absolute ON THE HOST
    // PLATFORM. "C:/project" is absolute only on Windows; on POSIX it is a
    // relative path named "C:", lexically_relative() against it yields an
    // empty path, and the function's "cannot express relative to the root"
    // guard then answers "not excluded" for every input — which makes the
    // whole fixture silently inert rather than merely failing.
    static std::filesystem::path WatchRoot()
    {
#ifdef _WIN32
        return "C:/project";
#else
        return "/project";
#endif
    }

    void SetUp() override
    {
        // StartWatching() would normally populate these; set them directly so
        // the test needs no on-disk watch directory.
        watcher.m_WatchDirectory = WatchRoot();
        watcher.m_NormalizedWatchBase = WatchRoot().lexically_normal();
    }

    // Cases are written relative to the watch root so they read identically on
    // every platform and cannot re-introduce a host-specific absolute path.
    bool IsExcluded(const std::string& relativePath)
    {
        return watcher.ShouldExcludeDirectory(WatchRoot() / relativePath);
    }

    // Same call, but on a path passed through verbatim instead of joined onto the watch
    // root. Every private access must sit in this class: FileWatcher befriends the fixture,
    // and friendship is not inherited, so a TEST_F body — a derived class — cannot reach
    // ShouldExcludeDirectory itself.
    bool IsExcludedVerbatim(const std::string& absolutePath)
    {
        return watcher.ShouldExcludeDirectory(absolutePath);
    }
};

// --- Files inside excluded directories (trailing-slash patterns) ---

TEST_F(FileWatcherExclusionTest, ExcludesFileInsideGitDir)
{
    EXPECT_TRUE(IsExcluded(".git/index"));
    EXPECT_TRUE(IsExcluded(".git/refs/heads/main"));
}

TEST_F(FileWatcherExclusionTest, ExcludesGitDirItself)
{
    // Windows can report a change on the directory entry with no trailing slash.
    EXPECT_TRUE(IsExcluded(".git"));
}

TEST_F(FileWatcherExclusionTest, ExcludesFileInsideVsDir)
{
    EXPECT_TRUE(IsExcluded(".vs/settings.json"));
}

TEST_F(FileWatcherExclusionTest, ExcludesVsDirItself)
{
    EXPECT_TRUE(IsExcluded(".vs"));
}

TEST_F(FileWatcherExclusionTest, ExcludesBuildDir)
{
    EXPECT_TRUE(IsExcluded("build/CMakeCache.txt"));
    EXPECT_TRUE(IsExcluded("build"));
}

TEST_F(FileWatcherExclusionTest, ExcludesBinAndObjDirs)
{
    EXPECT_TRUE(IsExcluded("bin/Debug/app.exe"));
    EXPECT_TRUE(IsExcluded("obj/Release/temp.o"));
    EXPECT_TRUE(IsExcluded("bin"));
    EXPECT_TRUE(IsExcluded("obj"));
}

TEST_F(FileWatcherExclusionTest, ExcludesCacheDir)
{
    EXPECT_TRUE(IsExcluded(".Cache/shaders/foo.spv"));
    EXPECT_TRUE(IsExcluded(".Cache"));
}

// --- Paths that should NOT be excluded ---

TEST_F(FileWatcherExclusionTest, DoesNotExcludeNormalSourceFile)
{
    EXPECT_FALSE(IsExcluded("src/main.cpp"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeGitignoreAtRoot)
{
    // .gitignore is NOT inside .git/ — must not be excluded.
    EXPECT_FALSE(IsExcluded(".gitignore"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeGitattributes)
{
    EXPECT_FALSE(IsExcluded(".gitattributes"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeSimilarlyNamedDir)
{
    // A directory named "git-hooks" should not match the ".git" exclusion.
    EXPECT_FALSE(IsExcluded("git-hooks/pre-commit"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeAssetsDir)
{
    EXPECT_FALSE(IsExcluded("Assets/texture.png"));
}

// --- Windows backslash paths ---

TEST_F(FileWatcherExclusionTest, ExcludesWithBackslashSeparators)
{
#ifdef _WIN32
    // ReadDirectoryChangesW reports native separators. Passed whole rather
    // than through IsExcluded(), which would join them onto the watch root.
    EXPECT_TRUE(IsExcludedVerbatim("C:\\project\\.git\\index"));
    EXPECT_TRUE(IsExcludedVerbatim("C:\\project\\.git"));
#else
    // A backslash is a legal filename character on POSIX, not a separator, so
    // "a\\b" is one component and this scenario cannot arise.
    GTEST_SKIP() << "Backslash separators are Windows-specific.";
#endif
}

// --- Demo/Resources exception ---

TEST_F(FileWatcherExclusionTest, AllowsDemoResourcesEvenInExcludedParent)
{
    // The special exception for Demo/Resources should take precedence.
    EXPECT_FALSE(IsExcluded("build/Demo/Resources/test.txt"));
}

// --- Nested excluded dirs ---

TEST_F(FileWatcherExclusionTest, ExcludesNestedGitDir)
{
    // A .git directory nested inside a submodule path.
    EXPECT_TRUE(IsExcluded("submodule/.git/HEAD"));
    EXPECT_TRUE(IsExcluded("submodule/.git"));
}

// --- Stop latency ---

// StopWatching wakes the watch loop and joins it. It returns within one wait
// slice (50 ms on the Windows backend, immediately on the polling backends),
// never after a fixed sleep and never by abandoning a thread that still uses
// the watcher. Hot-reload rebinds stop and restart watchers on the main thread.
TEST(FileWatcherStop, StopReturnsWellInsideThePollInterval)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_stop");
    ASSERT_TRUE(fs::create_directories(dir));

    constexpr uint32 kPollIntervalMs = 5000;
    constexpr auto kAllowed = std::chrono::milliseconds(100);

    FileWatcher watcher;
    watcher.SetPollingInterval(kPollIntervalMs);
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));

    // Let the initial scan finish so the stop lands in the loop's wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto start = std::chrono::steady_clock::now();
    watcher.StopWatching();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::error_code ec;
    fs::remove_all(dir, ec);

    EXPECT_FALSE(watcher.IsWatching());
    EXPECT_LT(elapsed, kAllowed)
        << "StopWatching took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms with a " << kPollIntervalMs << " ms poll interval";
}

// --- Detection latency ---

#ifdef PLATFORM_MACOS
// The polling loop's sleep is interruptible: on macOS an FSEvents stream wakes
// it, so a write is noticed in FSEvents time rather than in poll time. Worth
// pinning because the poll interval is what a shader hot-reload's
// edit-to-pixels latency was mostly made of.
TEST(FileWatcherLatency, ChangeIsDetectedWellInsideThePollInterval)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_latency");
    ASSERT_TRUE(fs::create_directories(dir));

    // Long enough that a poll-only watcher cannot pass by accident: the
    // assertion allows a fifth of it.
    constexpr uint32 kPollIntervalMs = 5000;
    constexpr auto kAllowed = std::chrono::milliseconds(kPollIntervalMs / 5);

    std::mutex m;
    std::condition_variable cv;
    bool seen = false;
    FileWatcher watcher;
    watcher.SetPollingInterval(kPollIntervalMs);
    watcher.SetCallback([&](const FileChangeEvent&) {
        std::lock_guard<std::mutex> lock(m);
        seen = true;
        cv.notify_all();
    });
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));

    // Let the initial scan finish so the write below is a change rather than
    // part of the baseline snapshot.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto start = std::chrono::steady_clock::now();
    {
        std::ofstream f(dir / "probe.txt");
        f << "hello";
    }

    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs * 2), [&] { return seen; });
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    watcher.StopWatching();
    std::error_code ec;
    fs::remove_all(dir, ec);

    ASSERT_TRUE(seen) << "the watcher never reported the write";
    EXPECT_LT(elapsed, kAllowed)
        << "detected in "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms with a " << kPollIntervalMs
        << " ms poll interval — the change signal is not waking the loop";
}
#endif // PLATFORM_MACOS

} // namespace GameEngine

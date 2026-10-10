#include "Assets/Packages/PackageNativeCache.h"
#include "Assets/Packages/PackageGitSource.h"
#include "FileSystem/ScopedFileLock.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#else
#include <process.h>
#include <windows.h>
#endif

using namespace GameEngine;
namespace fs = std::filesystem;

class PackageNativeCacheTest : public testing::Test
{
protected:
    TestUtils::ScopedTempDir m_Temporary{TestUtils::MakeUniqueTempDirectory("PackageNativeCache")};
    static constexpr std::string_view kDefaultPackage = "eztree-000000000001";
    static std::string Hash(unsigned value)
    {
        std::ostringstream hash;
        hash << std::hex << std::setfill('0') << std::setw(12) << value;
        return hash.str();
    }
    fs::path Entry(unsigned generation, std::string_view package = kDefaultPackage) const
    {
        return m_Temporary.Path() / ".native" / package / Hash(generation);
    }
    // The scope segment the real path builder derives for an entry name.
    std::string PackageScope(std::string_view entryName) const
    {
        const fs::path generation =
            PackageCacheDerivedNativeRoot(m_Temporary.Path(), entryName, "engine");
#if defined(_WIN32)
        EXPECT_EQ(generation.parent_path().parent_path(), fs::temp_directory_path() / "GameEngine" / ".native");
#else
        EXPECT_EQ(generation.parent_path().parent_path(), m_Temporary.Path() / ".native");
#endif
        EXPECT_EQ(generation.filename().string().size(), 12u);
        return generation.parent_path().filename().string();
    }
    std::unique_ptr<FileSystem::ScopedFileLock> Acquire(const fs::path& entry) const
    {
        return AcquirePackageNativeCache(entry, m_Temporary.Path());
    }
    static fs::path LockFile(const fs::path& entry)
    {
        return entry.parent_path().parent_path() / ".locks" /
               entry.parent_path().filename() / entry.filename();
    }
    void Populate(unsigned first, unsigned last, std::string_view package = kDefaultPackage)
    {
        for (unsigned generation = first; generation <= last; ++generation)
        {
            auto lease = Acquire(Entry(generation, package));
            ASSERT_NE(lease, nullptr);
            std::ofstream(Entry(generation, package) / "compiled-module") << "derived output";
            fs::last_write_time(LockFile(Entry(generation, package)),
                                fs::file_time_type::clock::now() - std::chrono::hours(100 - generation));
        }
    }
    static bool WaitForPath(const fs::path& path)
    {
        for (unsigned attempt = 0; attempt != 300; ++attempt)
        {
            if (fs::exists(path))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};

#if defined(_WIN32)
TEST_F(PackageNativeCacheTest, DeepPackageCacheUsesShortIsolatedNativeRoot)
{
    const auto cacheRoot = m_Temporary.Path() / std::string(120, 'd') / "GameEngine" / "PackageCache";
    constexpr std::string_view entryName = "diversion-vcs@1.0.0-0123456789ab";
    const auto nativeRoot = PackageCacheDerivedNativeRoot(cacheRoot, entryName, "engine");

    EXPECT_EQ(nativeRoot.parent_path().parent_path(),
              fs::temp_directory_path() / "GameEngine" / ".native");
    EXPECT_LT(nativeRoot.native().size(), cacheRoot.native().size());
    EXPECT_EQ(nativeRoot, PackageCacheDerivedNativeRoot(cacheRoot, entryName, "engine"));
    EXPECT_NE(nativeRoot, PackageCacheDerivedNativeRoot(cacheRoot / "other", entryName, "engine"));
    EXPECT_NE(nativeRoot, PackageCacheDerivedNativeRoot(cacheRoot, "other-package", "engine"));
    EXPECT_NE(nativeRoot, PackageCacheDerivedNativeRoot(cacheRoot, entryName, "other-engine"));
}
#endif

TEST_F(PackageNativeCacheTest, DeletedCacheRootScopesArePrunedAfterTheirLastLease)
{
    const auto deletedRoot = m_Temporary.Path() / "deleted-cache";
    const auto liveRoot = m_Temporary.Path() / "live-cache";
    fs::create_directories(deletedRoot);
    fs::create_directories(liveRoot);
    const auto inactive = Entry(1, "deleted-000000000002");
    const auto active = Entry(2, "deleted-000000000002");
    const auto live = Entry(1, "live-000000000003");
    ASSERT_NE(AcquirePackageNativeCache(inactive, deletedRoot), nullptr);
    auto lease = AcquirePackageNativeCache(active, deletedRoot);
    ASSERT_NE(lease, nullptr);
    ASSERT_NE(AcquirePackageNativeCache(live, liveRoot), nullptr);
    std::ofstream(inactive / "compiled-module") << "inactive";
    std::ofstream(active / "compiled-module") << "leased";
    std::ofstream(live / "compiled-module") << "live";

    fs::remove_all(deletedRoot);
    ASSERT_FALSE(fs::exists(deletedRoot));
    ASSERT_NE(AcquirePackageNativeCache(Entry(2, "live-000000000003"), liveRoot), nullptr);
    EXPECT_FALSE(fs::exists(inactive));
    EXPECT_TRUE(fs::exists(active / "compiled-module"));
    EXPECT_TRUE(fs::exists(live / "compiled-module"));

    lease.reset();
    ASSERT_NE(AcquirePackageNativeCache(Entry(3, "live-000000000003"), liveRoot), nullptr);
    EXPECT_FALSE(fs::exists(active.parent_path()));
    EXPECT_TRUE(fs::exists(live / "compiled-module"));
    EXPECT_TRUE(fs::exists(liveRoot));
}

TEST_F(PackageNativeCacheTest, DamagedRootRecordsDoNotAuthorizeCleanup)
{
    const auto liveRoot = m_Temporary.Path() / "live-cache";
    fs::create_directories(liveRoot);
    std::string embeddedNull = (m_Temporary.Path() / "missing").generic_string();
    embeddedNull.append("\0suffix", 7);
    const std::string malformed[] = {"", "relative/path", std::string(128 * 1024 + 1, 'x'),
                                      embeddedNull};
    unsigned index = 0;
    for (const auto& contents : malformed)
    {
        const auto deletedRoot = m_Temporary.Path() / ("deleted-" + std::to_string(index));
        fs::create_directories(deletedRoot);
        const auto entry = Entry(1, "damaged-" + Hash(index + 1));
        ASSERT_NE(AcquirePackageNativeCache(entry, deletedRoot), nullptr);
        std::ofstream(entry / "compiled-module") << "keep";
        std::ofstream record(entry.parent_path() / "cache-root", std::ios::binary | std::ios::trunc);
        record.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        record.close();
        fs::remove_all(deletedRoot);

        ASSERT_NE(AcquirePackageNativeCache(Entry(index + 1, "live-000000000003"), liveRoot), nullptr);
        EXPECT_TRUE(fs::exists(entry / "compiled-module")) << "record case " << index;
        ++index;
    }
}

TEST_F(PackageNativeCacheTest, RepeatedEngineBuildsRetainFourGenerations)
{
    Populate(1, 20);
    for (unsigned generation = 1; generation <= 20; ++generation)
        EXPECT_EQ(fs::exists(Entry(generation)), generation >= 17);
}

TEST_F(PackageNativeCacheTest, ActiveGenerationSurvivesAndIsCollectedAfterRelease)
{
    auto active = Acquire(Entry(1));
    ASSERT_NE(active, nullptr);
    fs::last_write_time(LockFile(Entry(1)),
                        fs::file_time_type::clock::now() - std::chrono::hours(100));
    Populate(2, 12);
    EXPECT_TRUE(fs::exists(Entry(1)));
    active.reset();
    Populate(13, 13);
    EXPECT_FALSE(fs::exists(Entry(1)));
}

TEST_F(PackageNativeCacheTest, SharedReadersExcludeEvictionUntilBothRelease)
{
    auto first = Acquire(Entry(1));
    auto second = Acquire(Entry(1));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    fs::last_write_time(LockFile(Entry(1)),
                        fs::file_time_type::clock::now() - std::chrono::hours(100));
    first.reset();
    Populate(2, 10);
    EXPECT_TRUE(fs::exists(Entry(1)));
    second.reset();
    Populate(11, 11);
    EXPECT_FALSE(fs::exists(Entry(1)));
}

TEST_F(PackageNativeCacheTest, LegacyAndUnrecognizedDirectoriesAreUntouched)
{
    const auto legacy = m_Temporary.Path() / ".derived" / "eztree-000000000001";
    const auto unrelated = Entry(1).parent_path() / "user-files";
    fs::create_directories(legacy);
    fs::create_directories(unrelated);
    Populate(1, 12);
    EXPECT_TRUE(fs::exists(legacy));
    EXPECT_TRUE(fs::exists(unrelated));
    EXPECT_EQ(Acquire(legacy), nullptr);
    EXPECT_EQ(Acquire(unrelated), nullptr);
    EXPECT_EQ(Acquire("relative/.native/eztree-000000000001"), nullptr);
}

TEST_F(PackageNativeCacheTest, SamePrefixPackagesHaveIndependentRetentionBudgets)
{
    // Two entries sharing an eight-character prefix land in distinct scopes,
    // and pruning one scope never reaches into the other.
    const std::string water = PackageScope("lunarsong-water@1.0.0-0123456789ab");
    const std::string grass = PackageScope("lunarsong-grass@1.0.0-0123456789ab");
    ASSERT_NE(water, grass);
    Populate(1, 5, water);
    Populate(1, 5, grass);
    for (unsigned generation = 1; generation <= 5; ++generation)
    {
        EXPECT_EQ(fs::exists(Entry(generation, water)), generation >= 2);
        EXPECT_EQ(fs::exists(Entry(generation, grass)), generation >= 2);
    }
}

TEST_F(PackageNativeCacheTest, BusyMaintenanceKeepsTheLeaseAndSkipsPruning)
{
    Populate(1, 4);
    const auto maintenance = Entry(1).parent_path().parent_path() / ".locks" / "maintenance";
    FileSystem::ScopedFileLock held(maintenance, FileSystem::ScopedFileLock::Mode::Exclusive, false);
    ASSERT_TRUE(held.IsLocked());
    const auto begin = std::chrono::steady_clock::now();
    auto lease = Acquire(Entry(5));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    ASSERT_NE(lease, nullptr);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_TRUE(fs::exists(Entry(1)));
}

TEST_F(PackageNativeCacheTest, RecentReusePreservesAnOldGeneration)
{
    Populate(1, 4);
    ASSERT_NE(Acquire(Entry(1)), nullptr);
    Populate(5, 5);
    EXPECT_TRUE(fs::exists(Entry(1)));
    EXPECT_FALSE(fs::exists(Entry(2)));
}

TEST_F(PackageNativeCacheTest, SymlinksCannotRedirectCleanupOrLockWrites)
{
    const auto outside = m_Temporary.Path() / "outside";
    fs::create_directories(outside);
    std::ofstream(outside / "keep") << "source";
    fs::create_directories(Entry(1).parent_path());
#if defined(_WIN32)
    const std::wstring command = L"cmd.exe /C mklink /J \"" + Entry(1).wstring() +
                                 L"\" \"" + outside.wstring() + L"\" >NUL";
    ASSERT_EQ(_wsystem(command.c_str()), 0);
#else
    fs::create_directory_symlink(outside, Entry(1));
#endif
    EXPECT_EQ(Acquire(Entry(1)), nullptr);
    Populate(2, 12);
    EXPECT_TRUE(fs::exists(outside / "keep"));
    const auto lock = LockFile(Entry(13));
#if defined(_WIN32)
    if (!::CreateSymbolicLinkW(lock.c_str(), (outside / "keep").c_str(), 0))
        GTEST_SKIP() << "Windows does not permit file symbolic links in this test environment";
#else
    fs::create_symlink(outside / "keep", lock);
#endif
    EXPECT_EQ(Acquire(Entry(13)), nullptr);
}

TEST_F(PackageNativeCacheTest, AnotherProcessLeaseProtectsOldBuild)
{
    Populate(1, 1);
#if defined(_WIN32)
    const auto ready = m_Temporary.Path() / "child-ready";
    const auto release = m_Temporary.Path() / "child-release";
    ASSERT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_ENTRY", Entry(1).string().c_str()), 0);
    ASSERT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_READY", ready.string().c_str()), 0);
    ASSERT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_RELEASE", release.string().c_str()), 0);
    wchar_t executable[MAX_PATH]{};
    ASSERT_GT(::GetModuleFileNameW(nullptr, executable, MAX_PATH), 0u);
    // The helper is DISABLED_ so the suite's own pass never runs it without the
    // environment the parent prepares here.
    const intptr_t child = _wspawnl(_P_NOWAIT, executable, executable, L"--gtest_also_run_disabled_tests",
                                    L"--gtest_filter=PackageNativeCacheChild.DISABLED_HoldsLeaseUntilReleased",
                                    nullptr);
    ASSERT_NE(child, -1);
    ASSERT_TRUE(WaitForPath(ready));
    Populate(2, 12);
    EXPECT_TRUE(fs::exists(Entry(1)));
    std::ofstream(release) << "release";
    int status = 0;
    EXPECT_EQ(_cwait(&status, child, 0), child);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_ENTRY", ""), 0);
    EXPECT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_READY", ""), 0);
    EXPECT_EQ(_putenv_s("GE_PACKAGE_CACHE_TEST_RELEASE", ""), 0);
    Populate(13, 13);
    EXPECT_FALSE(fs::exists(Entry(1)));
#else
    int ready[2], release[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(release), 0);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        close(ready[0]);
        close(release[1]);
        FileSystem::ScopedFileLock lease(LockFile(Entry(1)),
                                         FileSystem::ScopedFileLock::Mode::Shared, false);
        const char result = lease.IsLocked() ? 'Y' : 'N';
        static_cast<void>(write(ready[1], &result, 1));
        char finish;
        static_cast<void>(read(release[0], &finish, 1));
        _exit(result == 'Y' ? 0 : 1);
    }
    close(ready[1]);
    close(release[0]);
    char result = 'N';
    EXPECT_EQ(read(ready[0], &result, 1), 1);
    EXPECT_EQ(result, 'Y');
    Populate(2, 12);
    EXPECT_TRUE(fs::exists(Entry(1)));
    static_cast<void>(write(release[1], "x", 1));
    close(release[1]);
    close(ready[0]);
    int status = 0;
    EXPECT_EQ(waitpid(child, &status, 0), child);
    EXPECT_EQ(status, 0);
    Populate(13, 13);
    EXPECT_FALSE(fs::exists(Entry(1)));
#endif
}

#if defined(_WIN32)
TEST(PackageNativeCacheChild, DISABLED_HoldsLeaseUntilReleased)
{
    const char* entryText = std::getenv("GE_PACKAGE_CACHE_TEST_ENTRY");
    const char* readyText = std::getenv("GE_PACKAGE_CACHE_TEST_READY");
    const char* releaseText = std::getenv("GE_PACKAGE_CACHE_TEST_RELEASE");
    ASSERT_NE(entryText, nullptr);
    ASSERT_NE(readyText, nullptr);
    ASSERT_NE(releaseText, nullptr);
    const fs::path entry(entryText);
    const fs::path ready(readyText);
    const fs::path release(releaseText);
    const fs::path lock = entry.parent_path().parent_path() / ".locks" /
                          entry.parent_path().filename() / entry.filename();
    FileSystem::ScopedFileLock lease(lock, FileSystem::ScopedFileLock::Mode::Shared, false);
    ASSERT_TRUE(lease.IsLocked());
    std::ofstream(ready) << "ready";
    for (unsigned attempt = 0; attempt != 300 && !fs::exists(release); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(fs::exists(release));
}
#endif

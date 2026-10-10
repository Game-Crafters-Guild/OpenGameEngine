#include <gtest/gtest.h>

#include "FileSystem/FileSystem.h"
#include "FileSystem/TreeRemoval.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{

std::filesystem::path MakeTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("ge_filesystem_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(FileSystem, CopiesFileBytesAndCreatesParent)
{
    const auto root = MakeTempDir();
    const auto from = root / "src" / "hello.bin";
    const auto to = root / "dst" / "nested" / "hello.bin";
    std::filesystem::create_directories(from.parent_path());
    {
        std::ofstream out(from, std::ios::binary);
        out << "payload-42";
    }

    ASSERT_TRUE(GameEngine::FileSystem::CopyFileContents(from, to));
    EXPECT_EQ(ReadAll(to), "payload-42");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, CopiesEmptySourceFile)
{
    // A zero-byte source must copy as a zero-byte destination, not report failure.
    // `out << in.rdbuf()` sets failbit on an empty source; a seed project ships
    // empty .lock files, so a false here fails the whole PlantSeedProject copy.
    const auto root = MakeTempDir();
    const auto from = root / "src";
    std::filesystem::create_directories(from);
    { std::ofstream empty(from / "empty.lock"); }
    {
        std::ofstream a(from / "a.txt");
        a << "A";
    }

    const auto to = root / "dst";
    ASSERT_TRUE(GameEngine::FileSystem::CopyTree(from, to));
    EXPECT_TRUE(std::filesystem::exists(to / "empty.lock"));
    EXPECT_EQ(std::filesystem::file_size(to / "empty.lock"), 0u);
    EXPECT_EQ(ReadAll(to / "a.txt"), "A");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, CopiesNestedTree)
{
    const auto root = MakeTempDir();
    const auto from = root / "src";
    std::filesystem::create_directories(from / "sub");
    {
        std::ofstream a(from / "a.txt");
        a << "A";
        std::ofstream b(from / "sub" / "b.txt");
        b << "B";
    }

    const auto to = root / "dst" / "copied";
    ASSERT_TRUE(GameEngine::FileSystem::CopyTree(from, to));
    EXPECT_EQ(ReadAll(to / "a.txt"), "A");
    EXPECT_EQ(ReadAll(to / "sub" / "b.txt"), "B");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, ListDirectoriesIsSingleLevelAndSkipsFiles)
{
    const auto root = MakeTempDir();
    std::filesystem::create_directories(root / "alpha" / "inner");
    std::filesystem::create_directories(root / "beta");
    { std::ofstream file(root / "loose.txt"); }

    auto listed = GameEngine::FileSystem::ListDirectories(root);
    std::sort(listed.begin(), listed.end());

    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0], root / "alpha");
    EXPECT_EQ(listed[1], root / "beta");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, ListDirectoriesOfMissingRootIsEmpty)
{
    const auto root = MakeTempDir();
    EXPECT_TRUE(GameEngine::FileSystem::ListDirectories(root / "absent").empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, TreeRemovalRemovesNestedTree)
{
    const auto root = MakeTempDir();
    const auto tree = root / "project";
    std::filesystem::create_directories(tree / "Assets" / "deep");
    { std::ofstream file(tree / "Assets" / "deep" / "scene.scene"); }

    // Inline mode (0 workers) runs the removal synchronously, so the result is
    // observable as soon as the call returns.
    ::JobSystem::WorkStealingThreadPool inlinePool(0);
    GameEngine::FileSystem::TreeRemoval removal(inlinePool);
    removal.Remove(tree);

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(tree, ec));
    EXPECT_TRUE(std::filesystem::exists(root, ec));

    std::filesystem::remove_all(root, ec);
}

// A removal holds no compute worker: with the pool's only worker parked, the
// tree is still removed (on a blocking thread of the "Tree removals" channel).
TEST(FileSystem, TreeRemovalHoldsNoComputeWorker)
{
    const auto root = MakeTempDir();
    const auto tree = root / "project";
    std::filesystem::create_directories(tree / "Assets");
    { std::ofstream file(tree / "Assets" / "scene.scene"); }

    ::JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> workerParked{false};
    std::atomic<bool> release{false};
    pool.EnqueueWork([&]
    {
        workerParked.store(true);
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!workerParked.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    bool removed = false;
    {
        GameEngine::FileSystem::TreeRemoval removal(pool);
        removal.Remove(tree);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::error_code ec;
        while (std::filesystem::exists(tree, ec) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        removed = !std::filesystem::exists(tree, ec);
        release.store(true);
    }
    EXPECT_TRUE(removed) << "the removal waited for the parked compute worker";

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// A removal once asked for is never dropped: one still queued when its
// TreeRemoval is destroyed runs anyway. The pool's only blocking thread is held
// by another channel's job; the first removal takes the channel's one slot and
// waits for that thread, so the second waits in the channel's queue when the
// TreeRemoval is destroyed.
TEST(FileSystem, TreeRemovalQueuedAtDestructionStillRuns)
{
    const auto root = MakeTempDir();
    const auto first = root / "first";
    const auto second = root / "second";
    for (const auto& tree : {first, second})
    {
        std::filesystem::create_directories(tree / "Assets");
        std::ofstream file(tree / "Assets" / "scene.scene");
    }

    ::JobSystem::WorkStealingThreadPool pool(1, 1);
    auto holder = std::make_unique<::JobSystem::JobChannel>(
        pool, ::JobSystem::JobChannelDesc{.Name = "Test blocking thread holder", .MaxRunning = 1});
    std::mutex mutex;
    std::condition_variable changed;
    bool holding = false;
    bool release = false;
    ::JobSystem::TaskHandle held = holder->Submit([&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        holding = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return holding; }));
    }

    {
        GameEngine::FileSystem::TreeRemoval removal(pool);
        removal.Remove(first);
        removal.Remove(second);
    }
    std::error_code ec;
    const bool queuedAtDestruction = std::filesystem::exists(first, ec) && std::filesystem::exists(second, ec);

    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    held.Wait();
    holder.reset();
    pool.Shutdown();

    EXPECT_TRUE(queuedAtDestruction) << "a removal ran before its TreeRemoval was destroyed; the test proves nothing";
    EXPECT_FALSE(std::filesystem::exists(first, ec)) << "the removal holding the channel's slot was dropped";
    EXPECT_FALSE(std::filesystem::exists(second, ec)) << "a removal queued when its TreeRemoval was destroyed was dropped";
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, CopyToSelfPreservesSource)
{
    const auto root = MakeTempDir();
    const auto file = root / "source.txt";
    { std::ofstream out(file); out << "preserve this payload"; }
    EXPECT_FALSE(GameEngine::FileSystem::CopyFileContents(file, file));
    EXPECT_EQ(ReadAll(file), "preserve this payload");
    EXPECT_FALSE(GameEngine::FileSystem::CopyTree(file, file));
    EXPECT_EQ(ReadAll(file), "preserve this payload");
    std::filesystem::remove_all(root);
}

TEST(FileSystem, MissingSourcePreservesExistingDestination)
{
    const auto root = MakeTempDir();
    const auto to = root / "existing.txt";
    { std::ofstream out(to); out << "existing payload"; }
    EXPECT_FALSE(GameEngine::FileSystem::CopyFileContents(root / "missing.txt", to));
    EXPECT_EQ(ReadAll(to), "existing payload");
    std::filesystem::remove_all(root);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
TEST(FileSystem, NativeCopiesPreserveExecutablePermissions)
{
    const auto root = MakeTempDir();
    const auto from = root / "src" / "script.sh";
    std::filesystem::create_directory(from.parent_path());
    { std::ofstream out(from); out << "#!/bin/sh\nexit 0\n"; }
    const auto expected = std::filesystem::perms::owner_read |
                          std::filesystem::perms::owner_write |
                          std::filesystem::perms::owner_exec;
    std::filesystem::permissions(from, expected);
    ASSERT_TRUE(GameEngine::FileSystem::CopyFileContents(from, root / "single.sh"));
    EXPECT_EQ(std::filesystem::status(root / "single.sh").permissions(), expected);
    ASSERT_TRUE(GameEngine::FileSystem::CopyTree(from.parent_path(), root / "tree"));
    EXPECT_EQ(std::filesystem::status(root / "tree" / "script.sh").permissions(), expected);
    std::filesystem::remove_all(root);
}
#endif
TEST(FileSystem, PublishFileReplacesDestinationAndConsumesTemp)
{
    const auto root = MakeTempDir();
    const auto target = root / "cache" / "artifact.bin";
    const auto temp = root / "cache" / "artifact.bin.tmp";
    std::filesystem::create_directories(target.parent_path());
    { std::ofstream(target, std::ios::binary) << "old-bytes"; }
    { std::ofstream(temp, std::ios::binary) << "new-bytes-longer"; }

    ASSERT_TRUE(GameEngine::FileSystem::PublishFile(temp, target));
    EXPECT_EQ(ReadAll(target), "new-bytes-longer");

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(temp, ec));

    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, PublishFileCreatesAbsentDestination)
{
    const auto root = MakeTempDir();
    const auto target = root / "cache" / "fresh.bin";
    const auto temp = root / "cache" / "fresh.bin.tmp";
    std::filesystem::create_directories(target.parent_path());
    { std::ofstream(temp, std::ios::binary) << "first"; }

    ASSERT_TRUE(GameEngine::FileSystem::PublishFile(temp, target));
    EXPECT_EQ(ReadAll(target), "first");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, PublishFileWithMissingTempFailsWithoutRetryAndLeavesDestinationIntact)
{
    // The destination is never unlinked speculatively: a failed publish must not
    // be able to leave the caller with no file at all.
    const auto root = MakeTempDir();
    const auto target = root / "cache" / "kept.bin";
    std::filesystem::create_directories(target.parent_path());
    { std::ofstream(target, std::ios::binary) << "keep-me"; }

    const auto started = std::chrono::steady_clock::now();
    const bool published = GameEngine::FileSystem::PublishFile(root / "cache" / "absent.tmp", target);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(published);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 150)
        << "A missing source cannot recover during the 200 ms retry delay";
    EXPECT_EQ(ReadAll(target), "keep-me");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, PublishFileFromManyThreadsIntoOneDirectory)
{
    // The shape that deadlocks WASMFS: several threads publishing into one
    // directory while others resolve paths in the same tree. Native hosts must
    // stay correct under it, and the browser host must not stop making progress.
    const auto root = MakeTempDir();
    const auto dir = root / "cache" / "Tex";
    std::filesystem::create_directories(dir);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 40;
    std::atomic<int> published{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kPerThread; ++i)
                {
                    const auto target = dir / ("t" + std::to_string(t) + "-" + std::to_string(i) + ".bin");
                    auto temp = target;
                    temp += ".tmp";
                    { std::ofstream(temp, std::ios::binary) << "payload" << t << i; }
                    if (GameEngine::FileSystem::PublishFile(temp, target))
                        ++published;
                    std::ifstream probe(target, std::ios::binary);
                    (void)probe.peek();
                }
            });
    }
    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(published.load(), kThreads * kPerThread);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, PublishToSelfPreservesFile)
{
    const auto root = MakeTempDir();
    const auto file = root / "kept.bin";
    { std::ofstream(file, std::ios::binary) << "keep-me"; }
    EXPECT_TRUE(GameEngine::FileSystem::PublishFile(file, file));
    EXPECT_EQ(ReadAll(file), "keep-me");
    std::filesystem::remove_all(root);
}

TEST(FileSystem, FailedPublicationPreservesDestinationDirectory)
{
    const auto root = MakeTempDir();
    const auto target = root / "kept";
    const auto temp = root / "replacement.tmp";
    std::filesystem::create_directory(target);
    { std::ofstream(target / "sentinel.bin", std::ios::binary) << "keep-me"; }
    { std::ofstream(temp, std::ios::binary) << "replacement"; }
    EXPECT_FALSE(GameEngine::FileSystem::PublishFile(temp, target));
    EXPECT_EQ(ReadAll(target / "sentinel.bin"), "keep-me");
    EXPECT_FALSE(std::filesystem::exists(temp));
    std::filesystem::remove_all(root);
}

TEST(FileSystem, RemovesOnlyTheTemporaryFilesOfWritersNoLongerRunning)
{
    const auto dir = MakeTempDir();
    const auto target = dir / "store-1.gepage";
    // A writer that is gone (no process has this id), this process's own in-flight write, another
    // target's temporary file, and a file that is not a temporary sibling at all.
    const auto orphan = dir / "store-1.gepage.2147483600-77-0.tmp";
    const auto ownWrite = GameEngine::FileSystem::MakeTemporarySiblingPath(target);
    const auto otherTarget = dir / "scene.getbake.2147483600-77-0.tmp";
    const auto unrelated = dir / "store-1.gepage";
    for (const auto& path : {orphan, ownWrite, otherTarget, unrelated})
        std::ofstream(path, std::ios::binary) << "x";

    EXPECT_EQ(GameEngine::FileSystem::RemoveOrphanedTemporaryFiles(dir, ".gepage"), 1u);
    EXPECT_FALSE(std::filesystem::exists(orphan));
    EXPECT_TRUE(std::filesystem::exists(ownWrite));
    EXPECT_TRUE(std::filesystem::exists(otherTarget));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
    std::filesystem::remove_all(dir);
}

TEST(FileSystem, KeepsTheTemporaryFileOfAnotherRunningWriter)
{
    const auto dir = MakeTempDir();
#ifdef _WIN32
    const int liveWriter = 4; // the System process: always running, never this process
#else
    const int liveWriter = static_cast<int>(getppid()); // the process that started this test
#endif
    const auto file = dir / ("store-1.gepage." + std::to_string(liveWriter) + "-77-0.tmp");
    std::ofstream(file, std::ios::binary) << "x";
    EXPECT_EQ(GameEngine::FileSystem::RemoveOrphanedTemporaryFiles(dir, ".gepage"), 0u);
    EXPECT_TRUE(std::filesystem::exists(file)) << "a running writer's temporary file was removed";
    std::filesystem::remove_all(dir);
}

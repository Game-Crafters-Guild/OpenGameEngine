#pragma once

// Test support for the file-watching service: proving a watcher is live, and observing
// what it delivered.
//
// Watching starts asynchronously in a way a test has to account for.
// FileWatcher::StartWatching spawns the watch thread and returns
// (Engine/Modules/FileWatcher/Source/FileWatcher.cpp:148-152); the platform directory handle and the
// first change request are made on that thread (:422, :463). A file written between the
// return and that first request is never reported. So a test that arms the watcher and
// immediately saves can see no watcher event at all — and a test that only counts change
// drives then passes because nothing was watching, which is the worst outcome: a green
// run that proves nothing.
//
// WaitUntilWatchingIsArmed closes the window by demonstration rather than by sleeping,
// and FileEventWitness is the positive control: assert it saw the write before drawing a
// conclusion from what the pipeline did with it.

#include "Assets/FileWatchingService.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

namespace TestUtils
{

/// Counts the change events the watching service dispatched for one file. Subscribes for
/// the life of the object; safe to construct before the service is watching.
class FileEventWitness
{
  public:
    FileEventWitness(const std::filesystem::path& directory, std::filesystem::path file)
        : m_File(std::move(file))
    {
        GameEngine::FilePattern pattern(directory, ".*", {}, /*recursive=*/true);
        m_Subscription.emplace(GameEngine::FileWatchingService::GetInstance().Subscribe(
            pattern,
            [this](const GameEngine::FileChangeEvent& event)
            {
                if (event.Path == m_File)
                    m_Count.fetch_add(1, std::memory_order_release);
            }));
    }

    FileEventWitness(const FileEventWitness&) = delete;
    FileEventWitness& operator=(const FileEventWitness&) = delete;

    int Count() const { return m_Count.load(std::memory_order_acquire); }

  private:
    std::filesystem::path m_File;
    std::optional<GameEngine::FileWatchSubscription> m_Subscription;
    std::atomic<int> m_Count{0};
};

/// Block until the watcher covering @p directory is demonstrably delivering events, by
/// rewriting a sentinel until one of its own events comes back. Call after
/// FileWatchingService::StartWatching(). The sentinel is removed before returning, so it
/// is not one of the paths a test then makes claims about.
///
/// The sentinel is rewritten rather than written once: a write lost to the arming window
/// has nothing to re-trigger it, so a single write plus a wait would decide by luck.
/// False means the watcher never delivered anything, which no amount of waiting in the
/// test body will fix.
inline bool WaitUntilWatchingIsArmed(const std::filesystem::path& directory,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    // Not ".tmp": FileWatcher::ShouldWatchFile drops that extension, so a .tmp sentinel
    // could never come back and this would always report a dead watcher.
    const std::filesystem::path sentinel = directory / "watcher_arm_sentinel.bin";
    FileEventWitness witness(directory, sentinel);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool armed = false;
    while (!armed && std::chrono::steady_clock::now() < deadline)
    {
        {
            std::ofstream out(sentinel, std::ios::binary | std::ios::trunc);
            out << "arming";
        }

        const auto attemptDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!armed && std::chrono::steady_clock::now() < attemptDeadline)
        {
            armed = witness.Count() > 0;
            if (!armed)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::error_code ec;
    std::filesystem::remove(sentinel, ec);
    return armed;
}

} // namespace TestUtils

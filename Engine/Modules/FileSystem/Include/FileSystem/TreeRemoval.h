#pragma once

#include <filesystem>
#include <memory>

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::FileSystem
{

/// Removes directory trees without blocking the caller. One per service that
/// deletes trees, constructed after the job system and destroyed before it.
///
/// Native: each removal is a job of the cap-1 "Tree removals" channel on the
/// job system's blocking threads, so a large tree never holds a compute worker.
/// A removal once asked for is never dropped: one still queued when this
/// object is destroyed, or when the job system shuts down, still runs.
/// Completion is logged, never signalled.
///
/// Web: one native OPFS removeEntry that runs inside the browser; the job
/// system is not used, and only trees under the persistent storage mount can
/// be removed.
class TreeRemoval
{
  public:
    explicit TreeRemoval(::JobSystem::WorkStealingThreadPool& jobSystem);
    ~TreeRemoval();
    TreeRemoval(const TreeRemoval&) = delete;
    TreeRemoval& operator=(const TreeRemoval&) = delete;

    /// Remove `root` and everything under it, off the calling thread.
    void Remove(const std::filesystem::path& root);

  private:
    // The "Tree removals" channel; null on web.
    std::unique_ptr<::JobSystem::JobChannel> m_Channel;
};

} // namespace GameEngine::FileSystem

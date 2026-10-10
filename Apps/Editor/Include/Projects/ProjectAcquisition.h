#pragma once

#include "Projects/ProjectCatalog.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
}

namespace GameEngine::Editor
{

enum class ProjectAcquireState : uint8_t
{
    Idle,
    Running,
    Succeeded,
    Failed,
};

struct ProjectAcquireRequest
{
    ProjectCatalogEntry Entry;
    // Directory Local sources resolve against (the template catalog root).
    std::filesystem::path CatalogRoot;
    // Final project directory; must not exist yet.
    std::filesystem::path Destination;
    // Written to the new project's settings as project.displayName.
    std::string DisplayName;
    // Zip sources only: a .zip already on disk (Import → Archive). When set,
    // the worker extracts it directly instead of downloading Source.Url.
    std::filesystem::path LocalArchivePath;
};

struct ProjectAcquireStatus
{
    ProjectAcquireState State = ProjectAcquireState::Idle;
    std::filesystem::path Destination;
    std::string Error;
};

// Materializes a catalog entry into a project directory as a job of its cap-1
// "Project acquisition" channel on `jobSystem`'s blocking threads (a clone or a
// download waits on git and the network, so it holds no compute worker):
// Local = recursive copy of the template folder; Git = shallow (sparse when a
// subfolder is set) clone into cache staging, then move into place. Stamps
// project.displayName / project.template / project.origin into the new
// project's settings. The picker polls Poll() from its Update(). Destroy it
// before the job system: an acquisition still queued then is cancelled, a
// running one finishes into the shared status.
class ProjectAcquisition
{
public:
    explicit ProjectAcquisition(JobSystem::WorkStealingThreadPool& jobSystem);
    ~ProjectAcquisition();

    ProjectAcquisition(const ProjectAcquisition&) = delete;
    ProjectAcquisition& operator=(const ProjectAcquisition&) = delete;

    // Starts the worker. Returns false when one is already running.
    bool Start(ProjectAcquireRequest request);

    // Copies the latest status when it changed since the previous poll.
    bool Poll(ProjectAcquireStatus& outStatus);

    static bool IsGitAvailable();

private:
    // Environment-dependent values (editor paths, PATH probe) resolved on the
    // calling thread in Start(); the worker must not touch them itself.
    struct WorkerContext
    {
        std::filesystem::path StagingRoot;
        std::filesystem::path SettingsFile;
        std::filesystem::path GitExecutable;
    };

    // Heap block the worker captures. The job may outlive this object (it is
    // never joined), so status lives until the last lambda drops its ref.
    struct SharedState
    {
        std::mutex Mutex;
        ProjectAcquireStatus Status;
        bool StatusDirty = false;
        std::atomic<bool> Running{false};
    };

    static void PublishTo(SharedState& state, ProjectAcquireState status,
                          const std::filesystem::path& destination, std::string error);
    static void RunWorker(const std::shared_ptr<SharedState>& state,
                          ProjectAcquireRequest request, WorkerContext context);

    std::shared_ptr<SharedState> m_Shared = std::make_shared<SharedState>();
    std::unique_ptr<JobSystem::JobChannel> m_Channel;
};

} // namespace GameEngine::Editor

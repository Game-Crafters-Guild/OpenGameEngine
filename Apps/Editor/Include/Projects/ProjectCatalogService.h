#pragma once

#include "Projects/ProjectCatalog.h"

#include "JobSystem/TaskHandle.h"

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

enum class CommunityCatalogState : uint8_t
{
    Idle,     // never requested
    Fetching, // worker in flight
    Ready,    // Catalog valid (freshly fetched, or FromCache after a failure)
    Failed,   // fetch failed and no usable cache
};

struct CommunityCatalogSnapshot
{
    CommunityCatalogState State = CommunityCatalogState::Idle;
    ProjectCatalog Catalog;
    bool FromCache = false;
    std::string Error; // set when the last fetch failed (even if a cache is shown)
};

// Loads the staged template manifest and fetches/caches the community one.
// The community fetch is a job of the service's cap-1 "Project catalog" channel on
// `jobSystem`'s blocking threads (it waits on the network); the picker polls
// PollCommunity() from its Update(). The fetch writes into this object, so the
// destructor waits for a fetch in flight; destroy the service before the job system.
class ProjectCatalogService
{
public:
    explicit ProjectCatalogService(JobSystem::WorkStealingThreadPool& jobSystem);
    ~ProjectCatalogService();

    ProjectCatalogService(const ProjectCatalogService&) = delete;
    ProjectCatalogService& operator=(const ProjectCatalogService&) = delete;

    // Synchronous read of <installTemplatesRoot>/manifest.json. Returns false
    // (with outError) when the manifest is missing or malformed. outRootDir is
    // the directory Local sources resolve against.
    static bool LoadTemplateCatalog(ProjectCatalog& outCatalog,
                                    std::filesystem::path& outRootDir,
                                    std::string* outError);

    // The community manifest source: preference "communityManifestSource"
    // (an https URL or an absolute local path), falling back to the official
    // catalog URL.
    static std::string GetCommunityManifestSource();

    // Kick off an async fetch. No-op while one is already in flight.
    void RequestCommunityFetch();

    // Copies the latest snapshot into outSnapshot when it changed since the
    // previous poll; returns false (leaving outSnapshot untouched) otherwise.
    bool PollCommunity(CommunityCatalogSnapshot& outSnapshot);

private:
    void FetchWorker(std::string source, std::filesystem::path cacheRoot);
    void Publish(CommunityCatalogSnapshot snapshot);

    std::unique_ptr<JobSystem::JobChannel> m_Channel;
    // The latest fetch job; the destructor waits on it.
    JobSystem::TaskHandle m_Fetch;
    std::mutex m_Mutex;
    CommunityCatalogSnapshot m_Snapshot;
    bool m_SnapshotDirty = false;
    std::atomic<bool> m_FetchInFlight{false};
};

} // namespace GameEngine::Editor

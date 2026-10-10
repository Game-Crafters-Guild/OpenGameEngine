// The community catalog fetch is a job of the service's "Project catalog" channel:
// it holds no compute worker, and the service's destructor waits for a fetch in
// flight, because the fetch writes its snapshot into the service.

#include <gtest/gtest.h>

#include "JobSystem/WorkStealingThreadPool.h"
#include "Projects/ProjectCatalogService.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using GameEngine::Editor::CommunityCatalogSnapshot;
using GameEngine::Editor::CommunityCatalogState;
using GameEngine::Editor::ProjectCatalogService;

namespace
{

constexpr const char* kManifest =
    R"({"schemaVersion": 1, "name": "Test", "projects": [{"id": "test-project", "name": "Test Project"}]})";

// A user data root of its own (GE_EDITOR_USER_DATA_ROOT) whose preferences point the
// community catalog at `source`.
class ScopedUserDataRoot
{
  public:
    explicit ScopedUserDataRoot(const fs::path& source)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("ge_catalog_" + std::to_string(stamp));
        fs::create_directories(m_Root / "user");
        std::ofstream prefs(m_Root / "user" / "Preferences.json");
        prefs << R"({"communityManifestSource": ")" << source.generic_string() << R"("})";
        prefs.close();
        SetUserDataRoot((m_Root / "user").string().c_str());
    }
    ~ScopedUserDataRoot()
    {
        SetUserDataRoot(nullptr);
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }
    const fs::path& Root() const { return m_Root; }

  private:
    static void SetUserDataRoot(const char* value)
    {
#ifdef _WIN32
        _putenv_s("GE_EDITOR_USER_DATA_ROOT", value ? value : "");
#else
        if (value)
            setenv("GE_EDITOR_USER_DATA_ROOT", value, 1);
        else
            unsetenv("GE_EDITOR_USER_DATA_ROOT");
#endif
    }

    fs::path m_Root;
};

fs::path TempPath(const char* name)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string(name) + std::to_string(stamp));
}

} // namespace

// With the pool's only compute worker held, a fetch from a local manifest still
// completes: it runs on a blocking thread of the catalog's channel.
TEST(ProjectCatalogService, AFetchHoldsNoComputeWorker)
{
    const fs::path manifest = TempPath("ge_catalog_manifest_") += ".json";
    {
        std::ofstream out(manifest);
        out << kManifest;
    }
    ScopedUserDataRoot userData(manifest);

    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::promise<void> workerHeld;
    JobSystem::TaskHandle held = pool.Submit([&workerHeld, released = release.get_future().share()]() {
        workerHeld.set_value();
        released.wait();
    });
    workerHeld.get_future().wait();

    bool readyWhileHeld = false;
    {
        ProjectCatalogService service(pool);
        service.RequestCommunityFetch();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        CommunityCatalogSnapshot snapshot;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (service.PollCommunity(snapshot) && snapshot.State == CommunityCatalogState::Ready)
            {
                readyWhileHeld = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release.set_value();
        held.Wait();
        EXPECT_TRUE(readyWhileHeld) << "the fetch waited for the held compute worker";
        if (readyWhileHeld)
        {
            ASSERT_EQ(snapshot.Catalog.Entries.size(), 1u);
            EXPECT_EQ(snapshot.Catalog.Entries[0].Id, "test-project");
        }
    }
    std::error_code ec;
    fs::remove(manifest, ec);
}

// The fetch is held inside its read of the manifest (a FIFO with no writer yet).
// Destroying the service then waits for the fetch, which writes into the service,
// instead of freeing it under the fetch.
TEST(ProjectCatalogService, DestroyingTheServiceWaitsForTheFetchInFlight)
{
#ifdef _WIN32
    GTEST_SKIP() << "holds the fetch inside its read with a POSIX FIFO, which Windows has no equivalent of "
                    "for a plain file read";
#else
    const fs::path fifo = TempPath("ge_catalog_fifo_");
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    ScopedUserDataRoot userData(fifo);

    JobSystem::WorkStealingThreadPool pool(1);
    auto service = std::make_unique<ProjectCatalogService>(pool);
    service->RequestCommunityFetch();

    std::future<void> destroyed = std::async(std::launch::async, [&service]() { service.reset(); });
    const bool returnedWhileFetching =
        destroyed.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
    {
        std::ofstream writer(fifo);
        writer << kManifest;
    }
    destroyed.wait();
    EXPECT_FALSE(returnedWhileFetching)
        << "the service was freed while its fetch, which writes into it, was still in flight";
    std::error_code ec;
    fs::remove(fifo, ec);
#endif
}

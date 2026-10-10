// A project acquisition is a job of its "Project acquisition" channel: a clone, a
// download or a template copy holds no compute worker.

#include <gtest/gtest.h>

#include "JobSystem/WorkStealingThreadPool.h"
#include "Projects/ProjectAcquisition.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using GameEngine::Editor::ProjectAcquireRequest;
using GameEngine::Editor::ProjectAcquireState;
using GameEngine::Editor::ProjectAcquireStatus;
using GameEngine::Editor::ProjectAcquisition;
using GameEngine::Editor::ProjectSourceType;

namespace
{

void SetUserDataRoot(const char* value)
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

} // namespace

// With the pool's only compute worker held, a template copy still completes.
TEST(ProjectAcquisition, AnAcquisitionHoldsNoComputeWorker)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("ge_acquisition_" + std::to_string(stamp));
    fs::create_directories(root / "user");
    fs::create_directories(root / "templates" / "empty" / "Assets");
    {
        std::ofstream file(root / "templates" / "empty" / "Assets" / "readme.txt");
        file << "template";
    }
    SetUserDataRoot((root / "user").string().c_str());

    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::promise<void> workerHeld;
    JobSystem::TaskHandle held = pool.Submit([&workerHeld, released = release.get_future().share()]() {
        workerHeld.set_value();
        released.wait();
    });
    workerHeld.get_future().wait();

    ProjectAcquireStatus status;
    {
        ProjectAcquisition acquisition(pool);
        ProjectAcquireRequest request;
        request.Entry.Id = "empty";
        request.Entry.Name = "Empty";
        request.Entry.Source.Type = ProjectSourceType::Local;
        request.Entry.Source.Path = "empty";
        request.CatalogRoot = root / "templates";
        request.Destination = root / "projects" / "Acquired";
        request.DisplayName = "Acquired";
        ASSERT_TRUE(acquisition.Start(request));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (acquisition.Poll(status) && status.State != ProjectAcquireState::Running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release.set_value();
        held.Wait();
    }
    EXPECT_EQ(status.State, ProjectAcquireState::Succeeded)
        << "the acquisition waited for the held compute worker (" << status.Error << ")";
    EXPECT_TRUE(fs::exists(root / "projects" / "Acquired" / "Assets" / "readme.txt"));

    SetUserDataRoot(nullptr);
    std::error_code ec;
    fs::remove_all(root, ec);
}

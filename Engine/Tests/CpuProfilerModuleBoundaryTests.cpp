#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Core/CpuProfiler.h"
#include "Mathematics/Ray.h"
#include "Scene/FunctionRef.h"
#include "Scene/SceneTlas.h"
#include "Scene/TlasInstance.h"

// CpuProfiler is a process-wide singleton reached through
// CpuProfiler::Get(). The scope sites that record into it are compiled into
// Engine.dll; the debug-server handler and the profiler panel that read it are
// compiled into Editor.exe. This suite stands in for that split: the test
// executable is its own linked image against Engine's import lib, so a scope
// recorded inside Engine.dll is only observable here if both sides resolve the
// same instance.
//
// The probe scope is a real production one — SceneTlas::TraverseRay carries
// GE_CPU_PROFILE_SCOPE("SceneTlas.TraverseRay") and the Scene module's objects
// are spliced into Engine.dll. An empty TLAS needs no World, no engine
// initialization and no GPU.
namespace
{

using GameEngine::Profiling::CpuProfiler;

constexpr std::string_view kEngineScope = "SceneTlas.TraverseRay";

// The read path the debug server's get_cpu_profiler handler uses:
// CpuProfiler::Get().CopyFrameSamples() over the last completed frame's flat
// map, so every read here is preceded by the BeginFrame that closes the frame
// the scope ran in.
bool FindFlatSample(const std::vector<std::pair<std::string_view, CpuProfiler::Sample>>& flat,
                    std::string_view name,
                    CpuProfiler::Sample& out)
{
    const auto it = std::find_if(flat.begin(), flat.end(),
                                 [name](const auto& kv) { return kv.first == name; });
    if (it == flat.end())
        return false;
    out = it->second;
    return true;
}

bool TreeContains(const std::vector<CpuProfiler::TreeRow>& tree, std::string_view name)
{
    return std::any_of(tree.begin(), tree.end(),
                       [name](const CpuProfiler::TreeRow& row) { return row.name == name; });
}

// Drives the engine-side scope. Returns the leaf-visit count so the call
// cannot be optimized away.
GameEngine::uint32 RunEngineSideScope()
{
    GameEngine::Scene::SceneTlas tlas;
    GameEngine::Mathematics::Ray3D ray{};
    ray.origin = {0.0f, 0.0f, 0.0f};
    ray.direction = {0.0f, 0.0f, 1.0f};
    auto visit = [](const GameEngine::Scene::TlasInstance&, GameEngine::float32, GameEngine::float32)
    { return true; };
    return tlas.TraverseRay(ray, {}, visit);
}

class CpuProfilerModuleBoundaryFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        CpuProfiler& prof = CpuProfiler::Get();
        prof.SetEnabled(true);
        prof.ClearFrameHistory();
        // Anchors the main thread (first BeginFrame wins). Twice, because a
        // frame boundary swaps the live map into the readable one rather than
        // clearing it: one call would leave a previous test's samples where
        // this test's reader looks.
        prof.BeginFrame();
        prof.BeginFrame();
    }

    void TearDown() override
    {
        CpuProfiler& prof = CpuProfiler::Get();
        prof.SetEnabled(false);
        prof.ClearFrameHistory();
    }
};

// The suite's reason for existing: without a single exported accessor the flat
// map read here stays empty because the scope recorded into a different image's
// singleton.
TEST_F(CpuProfilerModuleBoundaryFixture, EngineDllScopeIsVisibleInTheFlatSamplesReadByTheDebugServer)
{
    ASSERT_TRUE(CpuProfiler::Get().IsEnabled());
    RunEngineSideScope();

    // Close the frame the scope ran in: the reader sees completed frames.
    CpuProfiler::Get().BeginFrame();

    std::vector<std::pair<std::string_view, CpuProfiler::Sample>> flat;
    CpuProfiler::Get().CopyFrameSamples(flat);

    CpuProfiler::Sample sample{};
    ASSERT_TRUE(FindFlatSample(flat, kEngineScope, sample))
        << "no '" << kEngineScope << "' sample: the scope recorded into a CpuProfiler instance "
        << "other than the one this image reads through Get()";
    EXPECT_GE(sample.count, 1u);
}

// The scope stack half. It is thread_local inside Engine.dll; a per-image copy
// would leave the call tree empty here even with a shared instance, which is
// what an empty get_cpu_profiler "tree" looked like.
TEST_F(CpuProfilerModuleBoundaryFixture, MainThreadEngineDllScopeLandsInTheCallTree)
{
    ASSERT_TRUE(CpuProfiler::Get().IsMainThread())
        << "fixture BeginFrame() did not anchor this thread as the main thread";
    RunEngineSideScope();

    // CopyFrameTree reads the previous frame, so close the frame the scope ran in.
    CpuProfiler::Get().BeginFrame();

    std::vector<CpuProfiler::TreeRow> tree;
    CpuProfiler::Get().CopyFrameTree(tree);
    EXPECT_TRUE(TreeContains(tree, kEngineScope))
        << "call tree has " << tree.size() << " rows and none named " << kEngineScope;
}

// BeginFrame runs in Engine.dll (Application::MainLoop) while the reader lives
// in another image. One instance means one frame boundary, and the reader is
// on the completed side of it: it sees the frame that just closed, that frame
// is retired by the next boundary, and totals never accumulate for the life of
// the process.
TEST_F(CpuProfilerModuleBoundaryFixture, TheReaderSeesTheLastCompletedFrame)
{
    RunEngineSideScope();
    RunEngineSideScope();
    CpuProfiler::Get().BeginFrame();

    std::vector<std::pair<std::string_view, CpuProfiler::Sample>> flat;
    CpuProfiler::Get().CopyFrameSamples(flat);
    CpuProfiler::Sample completed{};
    ASSERT_TRUE(FindFlatSample(flat, kEngineScope, completed))
        << "the frame that just closed is not what the reader gets";
    EXPECT_EQ(completed.count, 2u);

    // The frame after that: the empty one is now the completed frame and the
    // two samples are gone rather than still counted.
    CpuProfiler::Get().BeginFrame();
    CpuProfiler::Get().CopyFrameSamples(flat);
    CpuProfiler::Sample retired{};
    EXPECT_FALSE(FindFlatSample(flat, kEngineScope, retired))
        << "sample survived two frame boundaries with count " << retired.count;

    RunEngineSideScope();
    CpuProfiler::Get().BeginFrame();
    CpuProfiler::Get().CopyFrameSamples(flat);
    CpuProfiler::Sample next{};
    ASSERT_TRUE(FindFlatSample(flat, kEngineScope, next));
    EXPECT_EQ(next.count, 1u) << "counts accumulate across frames instead of resetting";
}

// Off-main scopes are why the flat aggregate matters: ScopedCpuProfile keeps
// them out of the main-thread call tree, so the ECS systems the scheduler
// dispatches to job workers are visible here and nowhere else. A reader that
// cannot tell them apart reports a worker's cost as main-thread cost.
TEST_F(CpuProfilerModuleBoundaryFixture, OffMainThreadScopeIsAttributedOffMain)
{
    std::thread worker([] { RunEngineSideScope(); });
    worker.join();
    CpuProfiler::Get().BeginFrame();

    std::vector<std::pair<std::string_view, CpuProfiler::Sample>> flat;
    CpuProfiler::Get().CopyFrameSamples(flat);
    CpuProfiler::Sample sample{};
    ASSERT_TRUE(FindFlatSample(flat, kEngineScope, sample))
        << "a scope that only ran off the main thread reached no reader";
    EXPECT_EQ(sample.count, 1u);
    EXPECT_EQ(sample.offMainCount, 1u) << "worker-thread work is reported as main-thread work";
}

// The same scope name on both threads in one frame. The aggregate merges them
// by name, so the split has to survive the merge or a mixed row reads as
// whichever thread happened to be checked.
TEST_F(CpuProfilerModuleBoundaryFixture, MainAndOffMainCallsToOneScopeStaySeparable)
{
    RunEngineSideScope();
    std::thread worker([] { RunEngineSideScope(); });
    worker.join();
    CpuProfiler::Get().BeginFrame();

    std::vector<std::pair<std::string_view, CpuProfiler::Sample>> flat;
    CpuProfiler::Get().CopyFrameSamples(flat);
    CpuProfiler::Sample sample{};
    ASSERT_TRUE(FindFlatSample(flat, kEngineScope, sample));
    EXPECT_EQ(sample.count, 2u);
    EXPECT_EQ(sample.offMainCount, 1u)
        << "the main-thread call and the worker call did not stay separable";
}

} // namespace

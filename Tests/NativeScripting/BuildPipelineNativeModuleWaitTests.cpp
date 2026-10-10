// BuildPipeline::Execute and the editor's native module builds: Execute waits for an
// in-flight module build before its first step (step 0), fails naming a module whose
// build failed, does not wait for an editor-only module (a game never ships one), and a
// cancel requested before Execute ends it at once, with no Tick.
// The module build is InFlightModuleBuild's real, fast-failing build. Execute runs with
// empty settings, so after step 0 it stops at the output-directory check without
// touching the engine.

#include "Engine/Build/BuildPipeline.h"

#include "InFlightModuleBuild.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;
using GameEngine::BuildPipeline;
using GameEngine::BuildProgress;
using GameEngine::BuildSettings;

namespace
{

// How long the completion callback holds the completion task. Longer than the
// waiter's cancel-check period, so a waiter that could read the build state while a
// Tick runs would return before the completion task finished.
constexpr std::chrono::milliseconds kCompletionDelay{300};

struct ScratchRoot
{
    fs::path Path;

    explicit ScratchRoot(const char* name) : Path(fs::temp_directory_path() / name)
    {
        std::error_code ec;
        fs::remove_all(Path, ec);
    }
    ~ScratchRoot()
    {
        std::error_code ec;
        fs::remove_all(Path, ec);
    }
};

// One Execute on its own thread, as the Build panel runs it. Progress is written by
// that thread and read after the join; Reported and Done are polled while it runs.
struct ExportRun
{
    std::atomic<bool> Reported{false};
    std::atomic<bool> Done{false};
    bool Result = true;
    int CompletionsAtReturn = -1;
    std::vector<std::string> Messages;
    BuildProgress Last;
};

void RunExecute(BuildPipeline& pipeline, ns::Testing::InFlightModuleBuild& build, ExportRun& run)
{
    run.Result = pipeline.Execute(BuildSettings{}, [&run](const BuildProgress& progress) {
        run.Messages.push_back(progress.statusMessage);
        run.Last = progress;
        run.Reported = true;
    });
    run.CompletionsAtReturn = build.Completions();
    run.Done = true;
}

void WaitFor(const std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline)
{
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

bool AnyContains(const std::vector<std::string>& messages, const std::string& needle)
{
    for (const std::string& message : messages)
        if (message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST(BuildPipelineNativeModuleWait, ExecuteWaitsForAnInFlightModuleBuildBeforeItsFirstStep)
{
    ScratchRoot root("ge_bpnmw_wait");
    ns::Testing::InFlightModuleBuild build(root.Path / "src", root.Path / "cache", root.Path / "scratch", "EZTree",
                                           ns::Testing::ModuleKind::Shipped,
                                           [] { std::this_thread::sleep_for(kCompletionDelay); });
    BuildPipeline pipeline(&build.Manager());
    ExportRun run;
    std::thread exportThread(RunExecute, std::ref(pipeline), std::ref(build), std::ref(run));

    // Tick (the editor's frame loop) only once Execute has reported, so an Execute
    // that does not wait has already returned before the build can complete.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    WaitFor(run.Reported, deadline);
    build.TickUntilCompleted(deadline);
    WaitFor(run.Done, deadline);
    if (!run.Done.load())
        pipeline.RequestCancel(); // releases a waiter the deadline left behind
    exportThread.join();

    ASSERT_EQ(build.Completions(), 1) << "the module build never completed";
    EXPECT_EQ(run.CompletionsAtReturn, 1) << "Execute returned before the module build completed";
    EXPECT_FALSE(run.Result);
    ASSERT_FALSE(run.Messages.empty());
    EXPECT_EQ(run.Messages.front(), "Waiting for native module 'EZTree' to finish building...");
    EXPECT_TRUE(AnyContains(run.Last.errors, "'EZTree' failed its latest build"))
        << "a failed waited-on build must fail the export by name";
    EXPECT_FALSE(run.Last.cancelled);
}

TEST(BuildPipelineNativeModuleWait, CancelBeforeExecuteReturnsWithoutATick)
{
    ScratchRoot root("ge_bpnmw_cancel");
    ns::Testing::InFlightModuleBuild build(root.Path / "src", root.Path / "cache", root.Path / "scratch", "EZTree",
                                           ns::Testing::ModuleKind::Shipped, nullptr);
    BuildPipeline pipeline(&build.Manager());
    pipeline.RequestCancel(); // the Build panel closing right after it started the build
    ExportRun run;
    std::thread exportThread(RunExecute, std::ref(pipeline), std::ref(build), std::ref(run));

    // No Tick: the closing editor's main thread is blocked joining the build thread.
    WaitFor(run.Done, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    const bool returnedWithoutTick = run.Done.load();
    if (!returnedWithoutTick)
        pipeline.RequestCancel(); // releases the waiter whose cancel was lost
    exportThread.join();

    EXPECT_TRUE(returnedWithoutTick) << "a cancel requested before Execute was lost; Execute waited for a Tick";
    EXPECT_FALSE(run.Result);
    EXPECT_TRUE(run.Last.cancelled);
    EXPECT_EQ(build.Completions(), 0);
}

TEST(BuildPipelineNativeModuleWait, ExecuteDoesNotWaitForAnEditorOnlyModuleBuild)
{
    ScratchRoot root("ge_bpnmw_editor_only");
    ns::Testing::InFlightModuleBuild build(root.Path / "src", root.Path / "cache", root.Path / "scratch",
                                           "EZTree.Editor", ns::Testing::ModuleKind::EditorOnly, nullptr);
    BuildPipeline pipeline(&build.Manager());
    ExportRun run;
    std::thread exportThread(RunExecute, std::ref(pipeline), std::ref(build), std::ref(run));

    // No Tick: the editor-only build stays in flight for the whole export.
    WaitFor(run.Done, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    const bool returnedWithoutTick = run.Done.load();
    if (!returnedWithoutTick)
        pipeline.RequestCancel(); // releases a waiter that waited for the editor-only build
    exportThread.join();

    EXPECT_TRUE(returnedWithoutTick) << "the export waited for an editor-only module build";
    EXPECT_EQ(run.CompletionsAtReturn, 0);
    EXPECT_FALSE(AnyContains(run.Messages, "Waiting for native module"));
    EXPECT_FALSE(AnyContains(run.Last.errors, "EZTree.Editor"));
    EXPECT_FALSE(run.Last.cancelled);
}

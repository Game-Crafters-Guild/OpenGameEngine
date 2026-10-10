#include "Jobs/HotReloadTasks.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include <future>
#include <fstream>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace GameEngine {

namespace {
// Generous: a cold compile server boots in seconds and a large project
// compiles for tens of seconds.
constexpr auto kRunTimeout = std::chrono::minutes(10);
constexpr auto kPollInterval = std::chrono::milliseconds(10);
} // namespace

HotReloadPipeline::HotReloadPipeline(JobSystem::WorkStealingThreadPool& threadPool, JobSystem::JobChannel& compiles)
    : m_ThreadPool(threadPool)
    , m_Compiles(compiles)
    , m_IsExecuting(false)
    , m_Progress(0.0f) {
    Logger::Log::Info("HotReloadPipeline initialized");
}

HotReloadPipeline::~HotReloadPipeline() {
    if (m_IsExecuting.load()) {
        Logger::Log::Info("Cancelling pipeline execution during destruction");
    }
    // Cancel even when idle: the last run can still have stages in flight after
    // it published its result (a failed stage ends monitoring early), and its
    // token makes the ones that have not started yet bail out at once.
    Cancel();
    JoinRuns();
    Logger::Log::Info("HotReloadPipeline destroyed");
}

void HotReloadPipeline::JoinRuns() {
    std::vector<std::future<void>> runs;
    {
        std::lock_guard<std::mutex> lock(m_RunsMutex);
        runs.swap(m_Runs);
    }
    for (auto& run : runs) {
        run.wait();
    }
}

std::future<HotReloadPipeline::PipelineStats> HotReloadPipeline::ExecuteAsync(
    const String& assemblyPath) {

    // Last-wins coalescing: if already executing, cancel and remember the newest request
    String chosenPath = assemblyPath;
    bool expectedFalse = false;
    if (!m_IsExecuting.compare_exchange_strong(expectedFalse, true)) {
        Logger::Log::Warning("Pipeline already executing: queuing latest request and cancelling current run");

        {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            m_PendingAssemblyPath = assemblyPath; // remember newest request
        }

        Cancel();

        auto waitStart = std::chrono::steady_clock::now();
        while (m_IsExecuting.load() &&
               (std::chrono::steady_clock::now() - waitStart) < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (m_IsExecuting.load()) {
            Logger::Log::Error("Previous pipeline operation did not complete within timeout, forcing execution");
            m_IsExecuting.store(false);
        }

        expectedFalse = false;
        if (!m_IsExecuting.compare_exchange_strong(expectedFalse, true)) {
            throw std::runtime_error("Failed to acquire pipeline execution lock after cancellation");
        }

        // Use the most recent pending request, if any
        {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            if (m_PendingAssemblyPath.has_value()) {
                chosenPath = *m_PendingAssemblyPath;
                m_PendingAssemblyPath.reset();
            }
        }
    }

    Logger::Log::Info("🚀 Starting async hot-reload pipeline for: {}", chosenPath);

    // Capture this run's token by value: the member is re-created per run, so
    // a run that lost a supersede race must keep observing ITS token, never a
    // successor's (which would let a zombie run monitor forever).
    auto runCancellation = std::make_shared<HotReloadCancellation>();
    auto runCancelToken = HotReloadCancellation::Token(runCancellation);
    {
        std::lock_guard<std::mutex> lock(m_CancellationMutex);
        m_Cancellation = runCancellation;
    }

    // Shared so the launcher lambda stays copyable for std::async.
    auto runResult = std::make_shared<std::promise<PipelineStats>>();
    std::future<PipelineStats> result = runResult->get_future();

    std::lock_guard<std::mutex> runsLock(m_RunsMutex);
    std::erase_if(m_Runs, [](const std::future<void>& run) {
        return run.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    m_Runs.push_back(std::async(std::launch::async, [this, chosenPath, runCancellation, runCancelToken, runResult]() {
        auto startTime = std::chrono::high_resolution_clock::now();
        PipelineStats stats;
        stats.success = false;
        uint64_t stageHandleGeneration = 0;
        // This run's own stage handles, waited on after the result is published.
        std::array<JobSystem::TaskHandle, 4> runStages;

        try {
            Logger::Log::Info("[Pipeline] Starting 4-stage async hot-reload pipeline");
            m_Progress.store(0.0f);

            Logger::Log::Info("[Pipeline] 🚀 Starting ASYNC hot-reload pipeline for <1ms main thread blocking");

            // Create per-pipeline preload sync for this run
            auto preloadSync = std::make_shared<PreloadSync>();
            preloadSync->ready.store(false);

            std::unique_ptr<CompilationTask> compilationTask;
            if (m_CompilerFactory) {
                compilationTask = std::make_unique<CompilationTask>(&m_ThreadPool, chosenPath, m_CompilerFactory(), runCancelToken);
            } else {
                compilationTask = std::make_unique<CompilationTask>(&m_ThreadPool, chosenPath, nullptr, runCancelToken);
            }

            // Stage 1 is a "Script compiles" channel job: it queues behind
            // any other compile of the process instead of meeting it at the
            // compile server's pipe, and it holds no compute worker while the
            // server compiles. A failed compile leaves Execute by exception,
            // which fails the handle with the compiler's message.
            auto compileStarted = std::make_shared<std::atomic<bool>>(false);
            JobSystem::TaskHandle compilationHandle =
                m_Compiles.Submit([compilation = std::move(compilationTask), compileStarted] {
                    compileStarted->store(true);
                    compilation->Execute();
                });
            runStages[0] = compilationHandle;
            {
                std::lock_guard<std::mutex> lock(m_StageHandlesMutex);
                stageHandleGeneration = ++m_StageHandleGeneration;
                m_StageHandles = {compilationHandle, {}, {}, {}};
            }
            // A Cancel() that raced the submission saw no handle; it already
            // set the token, so finish its job here on this run's own handle.
            if (runCancelToken->load(std::memory_order_relaxed)) {
                compilationHandle.Cancel();
            }
            Logger::Log::Info("[Pipeline] 🚀 Stage 1: compilation queued on the script compiles channel (TaskId: {})",
                              compilationHandle.GetId());

            if (std::optional<PipelineStats> endedAtCompile = AwaitCompilation(compilationHandle, *compileStarted, runCancelToken)) {
                stats = std::move(*endedAtCompile);
            } else {
                std::unique_ptr<FileLoadingTask> fileLoadTask;
                if (m_LoaderFactory) {
                    fileLoadTask = std::make_unique<FileLoadingTask>(&m_ThreadPool, chosenPath, m_LoaderFactory(), runCancelToken);
                } else {
                    fileLoadTask = std::make_unique<FileLoadingTask>(&m_ThreadPool, chosenPath, nullptr, runCancelToken);
                }
                const JobSystem::TaskId fileLoadId = fileLoadTask->GetTaskId();
                auto fileLoadHandle = m_ThreadPool.Submit(std::move(fileLoadTask));
                runStages[1] = fileLoadHandle;

                // AssemblyPrepTask reads the bytes from the completed FileLoadingTask's handle.
                auto assemblyPrepTask = std::make_unique<AssemblyPrepTask>(&m_ThreadPool, chosenPath, fileLoadHandle, preloadSync, runCancelToken);
                std::unique_ptr<MainThreadSwapTask> swapTask;
                if (m_SwapperFactory) {
                    swapTask = std::make_unique<MainThreadSwapTask>(&m_ThreadPool, chosenPath, nullptr, preloadSync, m_SwapperFactory(), runCancellation);
                } else {
                    swapTask = std::make_unique<MainThreadSwapTask>(&m_ThreadPool, chosenPath, nullptr, preloadSync, nullptr, runCancellation);
                }

                const JobSystem::TaskId assemblyPrepId = assemblyPrepTask->GetTaskId();
                const JobSystem::TaskId swapId = swapTask->GetTaskId();
                assemblyPrepTask->AddDependency(fileLoadId);
                swapTask->AddDependency(assemblyPrepId);

                auto assemblyPrepHandle = m_ThreadPool.Submit(std::move(assemblyPrepTask));
                auto swapHandle = m_ThreadPool.Submit(std::move(swapTask));
                runStages[2] = assemblyPrepHandle;
                runStages[3] = swapHandle;

                // Retain the stage handles so Cancel()/supersede can retire
                // queued stages via F12 instead of letting them run to a token
                // bail-out. A zombie run (forced-execution timeout path) leaves
                // a successor's handles alone.
                {
                    std::lock_guard<std::mutex> lock(m_StageHandlesMutex);
                    if (m_StageHandleGeneration == stageHandleGeneration) {
                        m_StageHandles = {compilationHandle, fileLoadHandle, assemblyPrepHandle, swapHandle};
                    }
                }

                // A Cancel() that raced these submissions saw only the compile
                // handle; finish its job on this run's own handles.
                if (runCancelToken->load(std::memory_order_relaxed)) {
                    fileLoadHandle.Cancel();
                    assemblyPrepHandle.Cancel();
                    swapHandle.Cancel();
                }

                Logger::Log::Info("[Pipeline] ✅ Tasks submitted to WorkStealingThreadPool: FileLoad={} → AssemblyPrep={} → Swap={}",
                                  fileLoadId, assemblyPrepId, swapId);

                stats = MonitorExecution(startTime, compilationHandle, fileLoadHandle, assemblyPrepHandle, swapHandle,
                                         runCancelToken);
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            stats.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            if (stats.success) {
                Logger::Log::Info("[Pipeline] ✅ Hot-reload pipeline completed successfully in {}ms", stats.totalDuration.count());
            } else {
                if (IsCancellation(stats.result)) {
                    Logger::Log::Info("[Pipeline] ℹ️ Hot-reload pipeline cancelled: {}", stats.errorMessage);
                } else {
                    Logger::Log::Error("[Pipeline] ❌ Hot-reload pipeline failed: {}", stats.errorMessage);
                }
            }

        } catch (const std::exception& e) {
            auto endTime = std::chrono::high_resolution_clock::now();
            stats.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            stats.success = false;
            stats.result = HotReloadResult::UnknownError;
            stats.errorMessage = e.what();

            Logger::Log::Error("[Pipeline] ❌ Hot-reload pipeline failed: {}", e.what());
        }

        // Release this run's stage handles before publishing "not executing";
        // the generation check keeps a zombie run (forced-execution timeout
        // path) from wiping a successor's freshly stored handles.
        {
            std::lock_guard<std::mutex> lock(m_StageHandlesMutex);
            const bool zombified = stageHandleGeneration != 0 && m_StageHandleGeneration != stageHandleGeneration;
            if (stageHandleGeneration != 0 && !zombified) {
                for (auto& handle : m_StageHandles) {
                    handle = JobSystem::TaskHandle{};
                }
            }
            // A zombie run must not release m_IsExecuting either: the
            // successor that bumped the generation owns it now, and a false
            // store here would let a third ExecuteAsync start concurrently
            // with that still-live, *uncancelled* successor — two runs
            // compiling and swapping at once. The successor releases the flag
            // when it finishes.
            if (!zombified) {
                m_IsExecuting.store(false);
            }
        }
        // Don't reset progress to 0.0f - leave it at the final value (1.0f for success, or intermediate value for failure)
        // This allows the test to read the final progress value after the pipeline completes

        runResult->set_value(std::move(stats));

        // A cancelled or failed run stops monitoring while a stage can still
        // be Running (a compile is never interrupted). Stay alive until every
        // stage of this run has finished, so JoinRuns() joins them all. Only
        // local handles are touched here.
        for (auto& stage : runStages) {
            stage.Wait();
        }
    }));

    return result;
}

void HotReloadPipeline::Cancel() {
    Logger::Log::Info("Hot-reload pipeline cancellation requested");

    // Order matters: the token is the correctness mechanism (it defuses any
    // stage that is already Running), the handle Cancel below is the
    // optimization (queued stages never run at all, F12). Setting the token
    // first guarantees MonitorExecution reports CancelledByNewOperation
    // rather than misreading a handle-cancelled stage as a stage failure.
    std::shared_ptr<HotReloadCancellation> cancellation;
    {
        std::lock_guard<std::mutex> lock(m_CancellationMutex);
        cancellation = m_Cancellation;
    }
    if (cancellation) {
        cancellation->Cancel(); // sets the token and wakes a swap waiting for the main thread
    }

    CancelStageHandles();

    m_Progress.store(0.0f);
}

void HotReloadPipeline::CancelStageHandles() {
    std::lock_guard<std::mutex> lock(m_StageHandlesMutex);
    for (auto& handle : m_StageHandles) {
        if (handle.IsValid()) {
            // F12 single-fire arbitration: true means the stage was still
            // queued and its body will never run; false means it is Running
            // (completes normally, defused by the token) or already terminal.
            handle.Cancel();
        }
    }
}

float HotReloadPipeline::GetProgress() const {
    return m_Progress.load();
}



std::optional<HotReloadPipeline::PipelineStats> HotReloadPipeline::AwaitCompilation(
    const JobSystem::TaskHandle& compilationHandle, const std::atomic<bool>& compileStarted,
    const std::shared_ptr<std::atomic<bool>>& cancelToken) {
    PipelineStats stats;
    stats.success = false;

    // A pool Shutdown racing this run's submission hands back an invalid
    // handle (post-gate Submit).
    if (!compilationHandle.IsValid()) {
        Logger::Log::Info("[Pipeline] ⚠️ Stage submission raced pool shutdown; treating run as cancelled");
        stats.result = HotReloadResult::Cancelled;
        stats.errorMessage = "Pipeline stages could not be submitted (thread pool shutting down)";
        return stats;
    }

    const auto deadline = std::chrono::steady_clock::now() + kRunTimeout;
    for (;;) {
        // The token first: a compile that ran to completion after a
        // supersede returns normally with its result defused.
        if (cancelToken->load(std::memory_order_relaxed)) {
            Logger::Log::Info("[Pipeline] ⚠️ Pipeline cancelled during compilation (superseded by newer change)");
            stats.result = HotReloadResult::CancelledByNewOperation;
            stats.errorMessage = "Superseded by newer change";
            return stats;
        }
        if (compilationHandle.IsDone()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            Logger::Log::Error("[Pipeline] ❌ Pipeline timeout after {}ms",
                               std::chrono::duration_cast<std::chrono::milliseconds>(kRunTimeout).count());
            stats.result = HotReloadResult::UnknownError;
            stats.errorMessage = "Pipeline execution timeout";
            return stats;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    if (compilationHandle.IsCompleted()) {
        m_Progress.store(0.25f);
        return std::nullopt;
    }
    // The handle's terminal status is published after the job's first
    // statement, so a job that ran reads compileStarted set here.
    if (!compileStarted.load()) {
        Logger::Log::Info("[Pipeline] ⚠️ Compilation cancelled before it ran (thread pool shutting down)");
        stats.result = HotReloadResult::Cancelled;
        stats.errorMessage = "Compilation cancelled before it ran (thread pool shutting down)";
        return stats;
    }
    Logger::Log::Error("[Pipeline] ❌ Compilation task failed");
    stats.result = HotReloadResult::CompilationFailed;
    stats.errorMessage = compilationHandle.GetErrorMessage();
    m_Progress.store(1.0f);
    return stats;
}

HotReloadPipeline::PipelineStats HotReloadPipeline::MonitorExecution(std::chrono::high_resolution_clock::time_point runStart,
                                                                    JobSystem::TaskHandle compilationHandle, JobSystem::TaskHandle fileLoadHandle,
                                                                    JobSystem::TaskHandle assemblyPrepHandle, JobSystem::TaskHandle swapHandle,
                                                                    const std::shared_ptr<std::atomic<bool>>& cancelToken) {
    PipelineStats stats;
    stats.success = false;

    // A pool Shutdown racing this run's submissions hands back invalid
    // handles (post-gate Submit). Polling those would spin until the
    // 10-minute timeout — report the run as cancelled instead.
    if (!compilationHandle.IsValid() || !fileLoadHandle.IsValid() ||
        !assemblyPrepHandle.IsValid() || !swapHandle.IsValid()) {
        Logger::Log::Info("[Pipeline] ⚠️ Stage submission raced pool shutdown; treating run as cancelled");
        stats.result = HotReloadResult::Cancelled;
        stats.errorMessage = "Pipeline stages could not be submitted (thread pool shutting down)";
        return stats;
    }

    try {
        const auto startTime = runStart;
        // Stage completion timepoints for finer-grained stats
        std::optional<std::chrono::high_resolution_clock::time_point> tCompilation;
        std::optional<std::chrono::high_resolution_clock::time_point> tFileLoad;
        std::optional<std::chrono::high_resolution_clock::time_point> tPrep;
        std::optional<std::chrono::high_resolution_clock::time_point> tSwap;


        Logger::Log::Info("[Pipeline] 🔄 Tasks will execute automatically: FileLoad → AssemblyPrep → Swap");

        // FIXED: Actually wait for all tasks to complete instead of just checking status once
        Logger::Log::Info("[Pipeline] ⏳ Waiting for all pipeline stages to complete...");

        // Wait for final task (swap) to complete - this ensures all dependencies are satisfied
        const auto timeout = kRunTimeout;
        auto waitStart = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - waitStart < timeout) {
            // Cooperative cancellation
            if (cancelToken && cancelToken->load(std::memory_order_relaxed)) {
                Logger::Log::Info("[Pipeline] ⚠️ Pipeline cancelled during monitoring (superseded by newer change)");
                stats.success = false;
                stats.result = HotReloadResult::CancelledByNewOperation;
                stats.errorMessage = "Superseded by newer change";
                break;
            }

            // Check if any task failed during execution first
            bool compilationFailed = compilationHandle.HasFailed();
            bool fileLoadFailed = fileLoadHandle.HasFailed();
            bool assemblyPrepFailed = assemblyPrepHandle.HasFailed();
            bool swapFailed = swapHandle.HasFailed();

            // A supersede that landed after this iteration's token check
            // shows up as handle-cancelled stages (HasFailed) — classify it
            // as cancellation, not as a stage failure.
            if ((compilationFailed || fileLoadFailed || assemblyPrepFailed || swapFailed) &&
                cancelToken && cancelToken->load(std::memory_order_relaxed)) {
                Logger::Log::Info("[Pipeline] ⚠️ Pipeline cancelled during monitoring (superseded by newer change)");
                stats.success = false;
                stats.result = HotReloadResult::CancelledByNewOperation;
                stats.errorMessage = "Superseded by newer change";
                break;
            }

            if (compilationFailed) {
                Logger::Log::Error("[Pipeline] ❌ Compilation task failed");
                stats.success = false;
                stats.result = HotReloadResult::CompilationFailed;
                stats.errorMessage = compilationHandle.GetErrorMessage();
                m_Progress.store(1.0f); // Set progress to 100% when pipeline fails
                break;
            } else if (fileLoadFailed) {
                Logger::Log::Error("[Pipeline] ❌ File loading task failed");
                stats.success = false;
                stats.result = HotReloadResult::FileLoadFailed;
                stats.errorMessage = fileLoadHandle.GetErrorMessage();
                m_Progress.store(1.0f); // Set progress to 100% when pipeline fails
                break;
            } else if (assemblyPrepFailed) {
                Logger::Log::Error("[Pipeline] ❌ Assembly preparation task failed");
                stats.success = false;
                stats.result = HotReloadResult::AssemblyPrepFailed;
                stats.errorMessage = assemblyPrepHandle.GetErrorMessage();
                m_Progress.store(1.0f); // Set progress to 100% when pipeline fails
                break;
            } else if (swapFailed) {
                Logger::Log::Error("[Pipeline] ❌ Assembly swap task failed");
                stats.success = false;
                stats.result = HotReloadResult::UnknownError;
                stats.errorMessage = swapHandle.GetErrorMessage();
                m_Progress.store(1.0f); // Set progress to 100% when pipeline fails
                break;
            }

            // Handle status is authoritative: the pool erases terminal tasks
            // from its dependency graph (slice 2), so per-id status polling
            // is gone — completion is observed via the TaskHandle.
            if (swapHandle.IsCompleted()) {
                if (!tCompilation && compilationHandle.IsCompleted()) tCompilation = std::chrono::high_resolution_clock::now();
                if (!tFileLoad && fileLoadHandle.IsCompleted()) tFileLoad = std::chrono::high_resolution_clock::now();
                if (!tPrep && assemblyPrepHandle.IsCompleted()) tPrep = std::chrono::high_resolution_clock::now();
                if (!tSwap) tSwap = std::chrono::high_resolution_clock::now();

                Logger::Log::Info("[Pipeline] ✅ All pipeline stages completed successfully");
                stats.success = true;
                stats.result = HotReloadResult::Success;
                m_Progress.store(1.0f);
                Logger::Log::Debug("[Pipeline] Progress set to 1.0f, current value: {}", m_Progress.load());
                break;
            }

            if (swapHandle.HasFailed()) {
                Logger::Log::Error("[Pipeline] ❌ Pipeline failed during execution");
                stats.success = false;
                stats.result = HotReloadResult::UnknownError;
                stats.errorMessage = "Pipeline execution failed";
                break;
            }

            // Record first-time completions for timing
            if (!tCompilation && compilationHandle.IsCompleted()) tCompilation = std::chrono::high_resolution_clock::now();
            if (!tFileLoad && fileLoadHandle.IsCompleted()) tFileLoad = std::chrono::high_resolution_clock::now();
            if (!tPrep && assemblyPrepHandle.IsCompleted()) tPrep = std::chrono::high_resolution_clock::now();

            // Update progress based on completed stages
            float progress = 0.0f;
            if (compilationHandle.IsCompleted()) progress += 0.25f;
            if (fileLoadHandle.IsCompleted()) progress += 0.25f;
            if (assemblyPrepHandle.IsCompleted()) progress += 0.25f;
            if (swapHandle.IsCompleted()) progress += 0.25f;
            m_Progress.store(progress);

            std::this_thread::sleep_for(kPollInterval);
        }

        // Only apply timeout logic if we exited the loop due to timeout (not due to task failure)
        if (!stats.success && stats.result == HotReloadResult::Success) {
            Logger::Log::Error("[Pipeline] ❌ Pipeline timeout after {}ms",
                               std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
            stats.errorMessage = "Pipeline execution timeout";
            stats.result = HotReloadResult::UnknownError;
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        stats.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Compute stage durations based on recorded timepoints, with reasonable fallbacks
        try {
            auto toMs = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d); };

            if (tCompilation) stats.compilationDuration = toMs(*tCompilation - startTime);
            if (tFileLoad)    stats.fileLoadDuration    = toMs(*tFileLoad - (tCompilation.value_or(startTime)));
            if (tPrep)        stats.assemblyPrepDuration= toMs(*tPrep - (tFileLoad.value_or(tCompilation.value_or(startTime))));

            // Swap: prefer actual duration from task result; otherwise infer from tSwap
            std::chrono::milliseconds actualSwapDuration;
            if (swapHandle.TryGetResult(actualSwapDuration)) {
                stats.swapDuration = actualSwapDuration;
            } else if (tSwap) {
                stats.swapDuration = toMs(*tSwap - (tPrep.value_or(tFileLoad.value_or(tCompilation.value_or(startTime)))));
            } else {
                stats.swapDuration = std::chrono::milliseconds(0);
            }

            // Assembly size is known from FileLoadingTask result if exposed; keep placeholder for now
            stats.assemblySize = stats.assemblySize ? stats.assemblySize : 0;
        } catch (const std::exception& e) {
            Logger::Log::Warning("[Pipeline] Error computing stage durations: {}", e.what());
        }

        if (stats.success) {
            Logger::Log::Info("[Pipeline] ✅ Hot-reload pipeline completed in {}ms", stats.totalDuration.count());
            Logger::Log::Info("[Pipeline] 📊 Stage timings: Compilation≈{}ms, FileLoad≈{}ms, AssemblyPrep≈{}ms, Swap≈{}ms",
                               stats.compilationDuration.count(),
                               stats.fileLoadDuration.count(),
                               stats.assemblyPrepDuration.count(),
                               stats.swapDuration.count());
        }

    } catch (const std::exception& e) {
        stats.success = false;
        stats.result = HotReloadResult::UnknownError;
        stats.errorMessage = e.what();
        Logger::Log::Error("[Pipeline] ❌ Exception during pipeline execution: {}", e.what());
    }

    // Note: Task cleanup is handled by WorkStealingThreadPool automatically
    // No need to manually remove tasks from dependency graph

    return stats;
}

} // namespace GameEngine

#pragma once

#include "JobSystem/TaskTypes.h"
#include "JobSystem/Task.h"
#include "Jobs/HotReloadCancellation.h"
#include "Logger/Logger.h"
#include "Types/Types.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include <mutex>

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{

// Forward declarations for dependency injection interfaces
namespace Jobs
{
class ICompiler;
class IAssemblyLoader;
class IAssemblySwapper;
} // namespace Jobs

// Use fully qualified JobSystem types to avoid conflicts

/**
 * @brief Result codes for hot-reload operations
 */
enum class HotReloadResult
{
    Success = 0,

    // Compilation errors
    CompilationFailed = -1,
    CompilationTimeout = -2,
    ProjectFileNotFound = -3,
    DotNetNotFound = -4,

    // File I/O errors
    FileLoadFailed = -10,
    FileNotFound = -11,
    FileAccessDenied = -12,
    FileCorrupted = -13,
    FileTooLarge = -14,

    // Assembly preparation errors
    AssemblyPrepFailed = -20,
    AssemblyInvalid = -21,
    AssemblyIncompatible = -22,
    ContextCreationFailed = -23,
    DependencyMissing = -24,

    // Swap errors
    SwapFailed = -30,
    SwapTimeout = -31,
    SwapRollbackFailed = -32,

    // System errors
    OutOfMemory = -40,
    ThreadPoolExhausted = -41,
    SystemResourcesUnavailable = -42,

    // Cancellation
    Cancelled = -100,
    CancelledByUser = -101,
    CancelledByTimeout = -102,
    CancelledByNewOperation = -103,

    // Unknown/unexpected errors
    UnknownError = -999
};

/**
 * @brief Convert HotReloadResult to human-readable string
 */
inline const char* HotReloadResultToString(HotReloadResult result)
{
    switch (result)
    {
    case HotReloadResult::Success:
        return "Success";
    case HotReloadResult::CompilationFailed:
        return "Compilation Failed";
    case HotReloadResult::CompilationTimeout:
        return "Compilation Timeout";
    case HotReloadResult::ProjectFileNotFound:
        return "Project File Not Found";
    case HotReloadResult::DotNetNotFound:
        return "DotNet Not Found";
    case HotReloadResult::FileLoadFailed:
        return "File Load Failed";
    case HotReloadResult::FileNotFound:
        return "File Not Found";
    case HotReloadResult::FileAccessDenied:
        return "File Access Denied";
    case HotReloadResult::FileCorrupted:
        return "File Corrupted";
    case HotReloadResult::FileTooLarge:
        return "File Too Large";
    case HotReloadResult::AssemblyPrepFailed:
        return "Assembly Preparation Failed";
    case HotReloadResult::AssemblyInvalid:
        return "Assembly Invalid";
    case HotReloadResult::AssemblyIncompatible:
        return "Assembly Incompatible";
    case HotReloadResult::ContextCreationFailed:
        return "Context Creation Failed";
    case HotReloadResult::DependencyMissing:
        return "Dependency Missing";
    case HotReloadResult::SwapFailed:
        return "Swap Failed";
    case HotReloadResult::SwapTimeout:
        return "Swap Timeout";
    case HotReloadResult::SwapRollbackFailed:
        return "Swap Rollback Failed";
    case HotReloadResult::OutOfMemory:
        return "Out Of Memory";
    case HotReloadResult::ThreadPoolExhausted:
        return "Thread Pool Exhausted";
    case HotReloadResult::SystemResourcesUnavailable:
        return "System Resources Unavailable";
    case HotReloadResult::Cancelled:
        return "Cancelled";
    case HotReloadResult::CancelledByUser:
        return "Cancelled By User";
    case HotReloadResult::CancelledByTimeout:
        return "Cancelled By Timeout";
    case HotReloadResult::CancelledByNewOperation:
        return "Cancelled By New Operation";
    case HotReloadResult::UnknownError:
        return "Unknown Error";
    default:
        return "Invalid Result Code";
    }
}

/**
 * @brief True when a hot-reload ended because it was cancelled, not because it failed.
 *
 * A run superseded by a newer file change reports failure, but that is the debounce
 * working as designed — logging it at error level buries the real failures during a
 * rapid-edit storm. Callers deciding a log severity must ask this rather than testing
 * errorMessage text: the messages differ per producer and are not a stable contract.
 */
inline bool IsCancellation(HotReloadResult result)
{
    switch (result)
    {
    case HotReloadResult::Cancelled:
    case HotReloadResult::CancelledByUser:
    case HotReloadResult::CancelledByTimeout:
    case HotReloadResult::CancelledByNewOperation:
        return true;
    default:
        return false;
    }
}

namespace Jobs
{

/**
 * @brief Interface for compilation operations
 */
class ICompiler
{
  public:
    virtual ~ICompiler() = default;

    struct CompilationResult
    {
        bool success;
        String output;
        std::vector<String> warnings;
        std::vector<String> errors;
    };

    /**
     * @brief Compile the scripts backing @p assemblyPath.
     *
     * @param cancelRequested Per-run supersede token (may be null). Cancel()
     * on a running compile never aborts it (F12: a running task completes),
     * so implementations must consult this token before any externally
     * visible side effect — in particular before writing the output DLL: a
     * superseded compile that ran anyway must never clobber the newer
     * compile's output on disk.
     */
    virtual CompilationResult compile(const String& assemblyPath, const std::atomic<bool>* cancelRequested) = 0;
};

/**
 * @brief Mutex serializing the final supersede check and the output-DLL write
 * across every writer of script assemblies (CompileServerCompiler,
 * ScriptManager::CompileProject and the prebuilt package-module copy).
 *
 * ScriptManager's "Script compiles" channel already runs its background
 * compiles one at a time. The writers it does not order are the synchronous
 * compiles on their caller's thread (RecompileAndReload, the initial build
 * inside Initialize when it is not deferred) and the prebuilt package copy;
 * each can land beside a channel compile that targets the same output path.
 */
std::mutex& AssemblyOutputWriteMutex();

/**
 * @brief Interface for assembly loading operations
 */
class IAssemblyLoader
{
  public:
    virtual ~IAssemblyLoader() = default;

    virtual std::vector<uint8_t> loadAssemblyBytes(const String& assemblyPath) = 0;
};

/**
 * @brief Interface for assembly swapping operations
 */
class IAssemblySwapper
{
  public:
    virtual ~IAssemblySwapper() = default;

    struct SwapResult
    {
        bool success;
        std::chrono::milliseconds duration;
        String errorMessage;
    };

    virtual SwapResult swapAssembly(const std::vector<uint8_t>& assemblyBytes, const String& assemblyPath) = 0;
};

} // namespace Jobs

/**
 * @brief Base class for all hot-reload pipeline tasks
 */
class HotReloadTask : public JobSystem::Task
{
  public:
    // Lightweight cancellation exception to short-circuit task work
    struct CancellationException : public std::exception
    {
        const char* what() const noexcept override { return "HotReload task cancelled"; }
    };

    HotReloadTask(JobSystem::WorkStealingThreadPool* jobSystem, const String& assemblyPath,
                  const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : JobSystem::Task(jobSystem), m_AssemblyPath(assemblyPath), m_StartTime(std::chrono::high_resolution_clock::now()), m_CancelToken(cancelToken) {}

    virtual ~HotReloadTask() = default;

    /**
     * @brief Get the assembly path being processed
     */
    const String& GetAssemblyPath() const { return m_AssemblyPath; }

    /**
     * @brief Get task execution duration
     */
    std::chrono::milliseconds GetExecutionDuration() const
    {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_StartTime);
    }

    /**
     * @brief Get the result of task execution
     */
    HotReloadResult GetResult() const { return m_Result; }

    /**
     * @brief Get error message if task failed
     */
    const String& GetErrorMessage() const { return m_ErrorMessage; }

  protected:
    /**
     * @brief Set the task result and notify thread pool
     */
    void SetResult(HotReloadResult result, const String& errorMessage = "")
    {
        m_Result = result;
        m_ErrorMessage = errorMessage;

        bool success = (result == HotReloadResult::Success);
        JobSystem::TaskId taskId = GetTaskId();
        Logger::Log::Debug("[HotReload] Task {} completed with result: {}",
                           taskId, success ? "success" : "failed");
    }

    /**
     * @brief Cooperative cancellation check
     */
    void CheckCancellation() const
    {
        if (m_CancelToken && m_CancelToken->load(std::memory_order_relaxed))
        {
            throw CancellationException();
        }
    }

  protected:
    String m_AssemblyPath;
    std::chrono::high_resolution_clock::time_point m_StartTime;
    HotReloadResult m_Result{HotReloadResult::Success};
    String m_ErrorMessage;
    std::shared_ptr<std::atomic<bool>> m_CancelToken;
};

/**
 * @brief Stage 1: Background assembly compilation task
 */
class CompilationTask : public HotReloadTask
{
  public:
    CompilationTask(JobSystem::WorkStealingThreadPool* jobSystem,
                    const String& assemblyPath,
                    std::unique_ptr<Jobs::ICompiler> compiler = nullptr,
                    const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(jobSystem, assemblyPath, cancelToken), m_Compiler(std::move(compiler))
    {
        if (!m_Compiler)
        {
            m_Compiler = CreateDefaultCompiler();
        }
    }

    /**
     * @brief Run the compiler.
     *
     * @throws std::runtime_error with the compiler's errors when the compile
     * fails, so the task handle reports Failed and the dependent stages never
     * run. A cancelled compile returns normally with result Cancelled.
     */
    void Execute() override;

    /**
     * @brief Get compilation output
     */
    const String& GetCompilationOutput() const { return m_CompilationOutput; }

    /**
     * @brief Get compilation warnings
     */
    const std::vector<String>& GetWarnings() const { return m_Warnings; }

    /**
     * @brief Get compilation errors
     */
    const std::vector<String>& GetErrors() const { return m_Errors; }

  private:
    /**
     * @brief Create default compiler implementation
     */
    std::unique_ptr<Jobs::ICompiler> CreateDefaultCompiler();

  private:
    String m_CompilationOutput;
    std::vector<String> m_Warnings;
    std::vector<String> m_Errors;
    std::unique_ptr<Jobs::ICompiler> m_Compiler;
};

/**
 * @brief Stage 2: Asynchronous file loading task
 */
class FileLoadingTask : public HotReloadTask
{
  public:
    FileLoadingTask(JobSystem::WorkStealingThreadPool* jobSystem, const String& assemblyPath,
                    std::unique_ptr<Jobs::IAssemblyLoader> loader = nullptr,
                    const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(jobSystem, assemblyPath, cancelToken), m_Loader(std::move(loader)) {}

    void Execute() override;

    /**
     * @brief Get loaded assembly bytes
     */
    const std::vector<uint8_t>& GetAssemblyBytes() const { return m_AssemblyBytes; }

    /**
     * @brief Get file size in bytes
     */
    size_t GetFileSize() const { return m_AssemblyBytes.size(); }

  private:
    /**
     * @brief Load file asynchronously with progress monitoring
     */
    bool LoadFileAsync();

    /**
     * @brief Validate assembly file format
     */
    bool ValidateAssemblyFormat() const;

  private:
    std::vector<uint8_t> m_AssemblyBytes;
    std::filesystem::file_time_type m_FileTimestamp;
    std::unique_ptr<Jobs::IAssemblyLoader> m_Loader;
};

// Per-pipeline synchronization for preloaded context readiness (replaces previous static sync)
struct PreloadSync
{
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> ready{false};
};

/**
 * @brief Stage 3: Background assembly preparation task
 */
class AssemblyPrepTask : public HotReloadTask
{
  public:
    AssemblyPrepTask(JobSystem::WorkStealingThreadPool* jobSystem, const String& assemblyPath, JobSystem::TaskHandle fileLoadTaskHandle,
                     std::shared_ptr<PreloadSync> preloadSync,
                     const std::shared_ptr<std::atomic<bool>>& cancelToken = nullptr)
        : HotReloadTask(jobSystem, assemblyPath, cancelToken), m_FileLoadTaskHandle(fileLoadTaskHandle), m_PreloadSync(std::move(preloadSync)) {}

    void Execute() override;

    /**
     * @brief Get prepared assembly context (only valid after successful execution)
     */
    void* GetPreparedContext() const { return m_PreparedContext; }

    /**
     * @brief Get assembly metadata
     */
    const String& GetAssemblyName() const { return m_AssemblyName; }
    const String& GetAssemblyVersion() const { return m_AssemblyVersion; }

  private:
    /**
     * @brief Create collectible assembly context
     */
    bool CreateCollectibleContext();

    /**
     * @brief Load assembly from memory stream
     */
    bool LoadAssemblyFromStream();

    /**
     * @brief Validate assembly integrity and compatibility
     */
    bool ValidateAssembly();

    /**
     * @brief Extract assembly metadata
     */
    void ExtractAssemblyMetadata();

  private:
    JobSystem::TaskHandle m_FileLoadTaskHandle;
    std::vector<uint8_t> m_AssemblyBytes; // Will be populated from FileLoadingTask
    void* m_PreparedContext{nullptr};
    void* m_PreparedAssembly{nullptr};
    String m_AssemblyName;
    String m_AssemblyVersion;
    std::vector<String> m_ExportedTypes;

  private:
    std::shared_ptr<PreloadSync> m_PreloadSync;
};

/**
 * @brief Stage 4: Minimal main-thread synchronization task
 */
class MainThreadSwapTask : public HotReloadTask
{
  public:
    MainThreadSwapTask(JobSystem::WorkStealingThreadPool* jobSystem, const String& assemblyPath, void* preparedContext,
                       std::shared_ptr<PreloadSync> preloadSync,
                       std::unique_ptr<Jobs::IAssemblySwapper> swapper = nullptr,
                       std::shared_ptr<HotReloadCancellation> cancellation = nullptr)
        : HotReloadTask(jobSystem, assemblyPath, HotReloadCancellation::Token(cancellation)), m_PreparedContext(preparedContext), m_Swapper(std::move(swapper)), m_PreloadSync(std::move(preloadSync)),
          m_Cancellation(cancellation ? std::move(cancellation) : std::make_shared<HotReloadCancellation>()) {}

    void Execute() override;

    /**
     * @brief Get swap duration (should be <1ms)
     */
    std::chrono::milliseconds GetSwapDuration() const { return m_SwapDuration; }

    /**
     * @brief Check if swap met performance target
     */
    bool MetPerformanceTarget() const { return m_SwapDuration.count() < 1; }

  private:
    /**
     * @brief Perform atomic assembly context swap
     */
    bool PerformAtomicSwap();

    /**
     * @brief Schedule old context cleanup on background thread
     */
    void ScheduleOldContextCleanup(void* oldContext);

    /**
     * @brief Update assembly references in engine systems
     */
    void UpdateAssemblyReferences();

  private:
    void* m_PreparedContext;
    void* m_OldContext{nullptr};
    std::chrono::milliseconds m_SwapDuration{0};
    bool m_SwapCompleted{false};
    std::unique_ptr<Jobs::IAssemblySwapper> m_Swapper;
    std::shared_ptr<PreloadSync> m_PreloadSync;
    // The run's cancellation (never null): the wait for the main thread ends
    // as soon as the run is cancelled.
    std::shared_ptr<HotReloadCancellation> m_Cancellation;
};

/**
 * @brief Hot-reload pipeline coordinator
 */
class HotReloadPipeline
{
  public:
    /**
     * @brief Pipeline execution statistics
     */
    struct PipelineStats
    {
        std::chrono::milliseconds totalDuration{0};
        std::chrono::milliseconds compilationDuration{0};
        std::chrono::milliseconds fileLoadDuration{0};
        std::chrono::milliseconds assemblyPrepDuration{0};
        std::chrono::milliseconds swapDuration{0};
        size_t assemblySize{0};
        bool success{false};
        HotReloadResult result{HotReloadResult::Success};
        String errorMessage;
    };

    /**
     * @param threadPool Runs the file load, preparation and swap stages.
     * @param compiles The process's "Script compiles" channel: the compile
     *        stage is a job of it, so it never runs beside another compile of
     *        the process at the compile server's pipe and never holds a
     *        compute worker while the server compiles. Must outlive the
     *        pipeline.
     */
    HotReloadPipeline(JobSystem::WorkStealingThreadPool& threadPool, JobSystem::JobChannel& compiles);

    /**
     * @brief Cancels the current run and joins every run this pipeline started.
     *
     * Blocks until every stage task of every run has finished, including a
     * stage that was already Running when it was cancelled or superseded (a
     * compile runs to completion). Stages hold what the factories gave them,
     * such as a compiler that points at its owner, so the owner must destroy
     * the pipeline before anything those stages reference.
     */
    ~HotReloadPipeline();

    /**
     * @brief Execute the complete hot-reload pipeline asynchronously
     * @param assemblyPath Path to the assembly to reload
     * @return Future that resolves when pipeline completes
     */
    std::future<PipelineStats> ExecuteAsync(const String& assemblyPath);

    // Dependency injection hooks for tests/tools
    void SetCompilerFactory(std::function<std::unique_ptr<Jobs::ICompiler>()> factory) { m_CompilerFactory = std::move(factory); }
    void SetAssemblyLoaderFactory(std::function<std::unique_ptr<Jobs::IAssemblyLoader>()> factory) { m_LoaderFactory = std::move(factory); }
    void SetSwapperFactory(std::function<std::unique_ptr<Jobs::IAssemblySwapper>()> factory) { m_SwapperFactory = std::move(factory); }

    /**
     * @brief Request cancellation of the current run and return without waiting.
     *
     * Queued stages never run. The run's future resolves as
     * CancelledByNewOperation once its monitor sees the token; a stage that is
     * already Running finishes in the background, and the destructor joins it.
     */
    void Cancel();

    /**
     * @brief Check if pipeline is currently executing
     */
    bool IsExecuting() const { return m_IsExecuting.load(); }

    /**
     * @brief Get current pipeline progress (0.0 to 1.0)
     */
    float GetProgress() const;

  private:
    /**
     * @brief Monitor pipeline execution and collect statistics
     * @param cancelToken The monitored run's own token — passed explicitly so
     * a run that lost a supersede race never polls a successor's token.
     */
    PipelineStats MonitorExecution(std::chrono::high_resolution_clock::time_point runStart,
                                   JobSystem::TaskHandle compilationHandle, JobSystem::TaskHandle fileLoadHandle,
                                   JobSystem::TaskHandle assemblyPrepHandle, JobSystem::TaskHandle swapHandle,
                                   const std::shared_ptr<std::atomic<bool>>& cancelToken);

    /**
     * @brief Wait for the compile stage, polling the run's own token so
     * Cancel and supersede resolve the run while the compile still runs.
     * `compileStarted` is set by the compile job's first statement: a handle
     * that ends neither Completed nor started was cancelled before the job ran
     * (the pool's drain, without the run's token) and reports Cancelled, not
     * CompilationFailed.
     * @return The run's result when it ends at the compile (cancelled,
     * failed, refused at the pool's shutdown, timed out); nothing when the
     * compile completed and the later stages should run.
     */
    std::optional<PipelineStats> AwaitCompilation(const JobSystem::TaskHandle& compilationHandle,
                                                  const std::atomic<bool>& compileStarted,
                                                  const std::shared_ptr<std::atomic<bool>>& cancelToken);

    /**
     * @brief Cancel the current run's stage handles (F12 arbitration).
     *
     * Queued stages never run, the compile included while it waits in the
     * "Script compiles" channel; a Running stage completes normally and its
     * result is defused by the per-run cancel token. The later stages are
     * submitted only after the compile completed.
     */
    void CancelStageHandles();

    /**
     * @brief Block until every run's launcher thread, and so every stage, has finished.
     */
    void JoinRuns();

  private:
    JobSystem::WorkStealingThreadPool& m_ThreadPool;
    JobSystem::JobChannel& m_Compiles;
    std::atomic<bool> m_IsExecuting{false};
    std::atomic<float> m_Progress{0.0f};

    // One launcher thread per ExecuteAsync. A launcher publishes its run's
    // result first, so Cancel and supersede stay prompt, and then waits for its
    // own stage tasks (the compile, and the three later stages when the compile
    // completed) before it exits. Joining the launchers therefore
    // joins every stage the pipeline submitted, including a superseded run's
    // compile that was still Running. ExecuteAsync appends (pruning finished
    // launchers) and the destructor joins. m_RunsMutex guards the vector:
    // ExecuteAsync can run on the file watcher and on the main thread. The
    // owner must not call ExecuteAsync once destruction has started.
    std::mutex m_RunsMutex;
    std::vector<std::future<void>> m_Runs;

    // The current run's cancellation, shared with that run's stages.
    // ExecuteAsync replaces it and Cancel reads it, and the two can run on
    // different threads (the file watcher starts runs, the main thread and the
    // destructor cancel them), so every access holds m_CancellationMutex. Each
    // run keeps its own copy, so it never observes a successor's cancellation.
    std::mutex m_CancellationMutex;
    std::shared_ptr<HotReloadCancellation> m_Cancellation;

    // The current run's stage handles (Compilation, FileLoad, AssemblyPrep,
    // Swap; the last three are invalid until the compile has completed),
    // retained so Cancel()/supersede can retire queued stages instantly
    // instead of letting each run to a token bail-out.
    // Generation-tagged so a zombie run (forced-execution timeout path)
    // cannot clear a newer run's handles.
    std::mutex m_StageHandlesMutex;
    std::array<JobSystem::TaskHandle, 4> m_StageHandles;
    uint64_t m_StageHandleGeneration{0};

    // Optional factories (used primarily in tests)
    std::function<std::unique_ptr<Jobs::ICompiler>()> m_CompilerFactory;
    // Last-wins pending request coalescing when multiple triggers arrive while executing
    std::mutex m_PendingMutex;
    std::optional<String> m_PendingAssemblyPath;

    std::function<std::unique_ptr<Jobs::IAssemblyLoader>()> m_LoaderFactory;
    std::function<std::unique_ptr<Jobs::IAssemblySwapper>()> m_SwapperFactory;

    DISALLOW_COPY_AND_ASSIGN(HotReloadPipeline);
};

} // namespace GameEngine

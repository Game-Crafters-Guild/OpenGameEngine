#include "Assets/AssetTasks.h"
#include "Assets/AssetDbProfiler.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetIOService.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AudioAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/RenderPipelineAsset.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{

namespace
{

// A cancelled decode is a withdrawn request, not a failure: a shutdown landing
// on a cold texture cache abandons every in-flight decode at once, and reporting
// each of those as an error would bury the real ones. Same discriminator the
// load-completion callbacks apply before marking a load suppressed (see
// AssetManager::SubmitInFlightLoad).
template <typename... Args>
void LogDecodeFailure(const std::shared_ptr<std::atomic<bool>>& cancelRequested,
                      std::format_string<Args...> fmt, Args&&... args)
{
    const std::string message = std::format(fmt, std::forward<Args>(args)...);
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire))
        Logger::Log::Debug(message);
    else
        Logger::Log::Error(message);
}

std::string GetAssetLogLabel(const GUID& assetGuid,
                             const std::filesystem::path& assetPath,
                             const AssetManager* assetManager)
{
    const std::string guidStr = assetGuid.ToString();

    // If we don't have any path information at all, surface that explicitly.
    if (assetPath.empty())
    {
        if (assetManager)
        {
            return "Unknown asset (GUID=" + guidStr + ", empty path)";
        }

        return "Unknown asset (GUID=" + guidStr + ")";
    }

    std::error_code ec;

    std::filesystem::path absPath = std::filesystem::absolute(assetPath, ec);
    if (ec)
    {
        // Fall back to the original path if we cannot make it absolute.
        absPath = assetPath;
    }
    absPath = absPath.lexically_normal();

    std::string display = absPath.generic_string();

    auto tryRelToRoot = [&](const std::filesystem::path& root) -> bool
    {
        if (!assetManager || root.empty())
        {
            return false;
        }

        std::error_code rec;
        std::filesystem::path absRoot = std::filesystem::absolute(root, rec);
        if (rec)
        {
            return false;
        }

        absRoot = absRoot.lexically_normal();

        auto rel = std::filesystem::relative(absPath, absRoot, rec);
        const std::string relDisplay = rel.generic_string();
        if (rec || relDisplay.empty() || relDisplay == "." || relDisplay.rfind("..", 0) == 0)
        {
            return false;
        }

        display = relDisplay;
        return true;
    };

    if (assetManager)
    {
        // Prefer primary project asset root
        if (!tryRelToRoot(assetManager->GetAssetRoot()))
        {
            // Fall back to registered non-project asset sources.
            for (const auto& source : assetManager->GetRegisteredSources())
            {
                if (tryRelToRoot(source.Root))
                {
                    break;
                }
            }
        }
    }

    std::string result = display;
    result += " (";
    result += guidStr;
    result += ")";
    return result;
}

// ========================================
// MERGED PROCESSING + REGISTRATION JOB
// ========================================
//
// The CPU half of the load pipeline. Runs as one JobPriority::Background pool
// job submitted by the AssetIOService after the blocking read completes
// (read-then-submit — no dependency-graph edges). Type-specific decode below;
// each path constructs the asset, calls LoadFromData (or a type-specific
// variant), then invokes Asset::PostLoad so derived classes can run
// data-dependent build steps. The same PostLoad hook also fires on
// Asset::Reload, keeping initial-load and hot-reload paths in sync.

SharedPtr<Asset> ProcessAudioAsset(AssetManager& assetManager,
                                   const AssetMetadata& metadata, const Vector<uint8>& data)
{
    SharedPtr<Asset> asset = assetManager.CreateAsset(metadata);
    if (!asset)
    {
        LogDecodeFailure(CurrentAssetDecodeCancellation(), "AssetDecodeJob: Failed to create Audio asset instance for '{}'",
                         metadata.Path.string());
        return SharedPtr<Asset>{};
    }

    // Load audio data with format-specific processing and per-asset runtime settings.
    // These settings are stored in the authoritative AssetRegistry (KV metadata).
    if (auto* audio = dynamic_cast<AudioAsset*>(asset.get()))
    {
        AudioAsset::RuntimeSettings s{};
        try
        {
            const auto& path = metadata.Path;
            AssetRegistry& reg = assetManager.GetRegistry();

            std::string v;
            if (reg.TryGetMetaValue(path, kAudioLoadPolicyMetaKey, v))
            {
                std::string lower = v;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
                if (lower == "decodetopcm" || lower == "decode" || lower == "pcm")
                    s.loadPolicy = AudioLoadPolicy::DecodeToPCM;
                else if (lower == "stream")
                    s.loadPolicy = AudioLoadPolicy::Stream;
                else
                    s.loadPolicy = AudioLoadPolicy::Auto;
            }

            if (reg.TryGetMetaValue(path, kAudioAllowVirtualizationMetaKey, v))
            {
                std::string lower = v;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
                s.allowVirtualization = !(lower == "0" || lower == "false" || lower == "no" || lower == "off");
            }
        }
        catch (...)
        {
            // ignore metadata issues; fall back to defaults
        }

        if (!audio->LoadFromDataWithRuntimeSettings(data, s, metadata.FileSize))
        {
            LogDecodeFailure(CurrentAssetDecodeCancellation(), "AssetDecodeJob: Failed to load Audio data from '{}'",
                             metadata.Path.string());
            return SharedPtr<Asset>{};
        }
    }
    else
    {
        // Fallback for legacy audio asset types (if any).
        if (!asset->LoadFromData(data))
        {
            LogDecodeFailure(CurrentAssetDecodeCancellation(), "AssetDecodeJob: Failed to load Audio data from '{}'",
                             metadata.Path.string());
            return SharedPtr<Asset>{};
        }
    }

    asset->PostLoad();
    return asset;
}

SharedPtr<Asset> ProcessTypedAsset(AssetManager& assetManager,
                                   const AssetMetadata& metadata, const Vector<uint8>& data)
{
    const std::string typeName{AssetTypeToString(metadata.Type)};

    SharedPtr<Asset> asset = assetManager.CreateAsset(metadata);
    if (!asset)
    {
        LogDecodeFailure(CurrentAssetDecodeCancellation(), "AssetDecodeJob: Failed to create {} asset instance for '{}'",
                         typeName, metadata.Path.string());
        return SharedPtr<Asset>{};
    }

    if (!asset->LoadFromData(data))
    {
        LogDecodeFailure(CurrentAssetDecodeCancellation(), "AssetDecodeJob: Failed to load {} data from '{}'",
                         typeName, metadata.Path.string());
        return SharedPtr<Asset>{};
    }

    asset->PostLoad();
    return asset;
}

SharedPtr<Asset> ProcessGenericAsset(AssetManager& assetManager, const GUID& assetGuid,
                                     const AssetMetadata& metadata, const Vector<uint8>& data)
{
    SharedPtr<Asset> asset = assetManager.CreateAsset(metadata);
    if (!asset)
    {
        Logger::Log::Error("AssetDecodeJob: Failed to create generic asset instance"
                           " (typeId='{}', type='{}', path='{}', guid={})",
                           std::string(metadata.TypeId),
                           std::string(AssetTypeToString(metadata.Type)),
                           metadata.Path.string(),
                           assetGuid.ToString());
        return SharedPtr<Asset>{};
    }

    // Load asset data with basic processing
    if (!asset->LoadFromData(data))
    {
        // Registry keys can be case-folded. Resolve the file's spelling only on
        // this failure path; if the file vanished after the read, retain its path.
        std::error_code pathError;
        const auto sourcePath = std::filesystem::canonical(metadata.Path, pathError);
        std::string reason = "Failed to load '" +
            (pathError ? metadata.Path : sourcePath).generic_string() + "'";
        const std::vector<std::string>* errors = nullptr;
        std::string_view errorPrefix;
        if (const auto* pipeline = dynamic_cast<const RenderPipelineAsset*>(asset.get()))
        {
            errors = &pipeline->GetErrors();
            errorPrefix = "RenderPipelineAsset: ";
        }
        else if (const auto* material = dynamic_cast<const MaterialAsset*>(asset.get()))
        {
            errors = &material->GetErrors();
            errorPrefix = "MaterialAsset: ";
        }
        if (errors && !errors->empty())
        {
            constexpr size_t kMaxReportedErrors = 6;
            const size_t count = std::min(errors->size(), kMaxReportedErrors);
            for (size_t index = 0; index < count; ++index)
            {
                std::string_view detail = (*errors)[index];
                if (detail.starts_with(errorPrefix))
                    detail.remove_prefix(errorPrefix.size());
                while (!detail.empty() && detail.back() == '.')
                    detail.remove_suffix(1);
                reason += index == 0 ? ": " : "; ";
                reason += detail;
            }
            if (errors->size() > count)
                reason += "; additional errors omitted";
        }
        // If this asset is being requested repeatedly (e.g. pipeline selection each frame),
        // suppress subsequent load attempts until the file changes to avoid log spam and
        // runaway async work.
        assetManager.SuppressLoadRetries(assetGuid, metadata, reason);

        // Do not throw: worker threads should treat this as a load failure and keep the Editor running.
        Logger::Log::Error("AssetDecodeJob: {}", reason);
        return SharedPtr<Asset>{};
    }

    asset->PostLoad();
    return asset;
}

SharedPtr<Asset> ProcessAssetData(AssetManager& assetManager, const GUID& assetGuid,
                                  const AssetMetadata& metadata, const Vector<uint8>& data)
{
    // Route to asset-type-specific processing based on metadata
    switch (metadata.Type)
    {
    case AssetType::Texture:
    case AssetType::Model:
    case AssetType::UILayout:
    case AssetType::UIStyle:
        return ProcessTypedAsset(assetManager, metadata, data);
    case AssetType::Audio:
        return ProcessAudioAsset(assetManager, metadata, data);
    default:
        return ProcessGenericAsset(assetManager, assetGuid, metadata, data);
    }
}

SharedPtr<Asset> ExecuteAssetDecodeJob(AssetManager& assetManager, const GUID& assetGuid,
                                       const AssetMetadata& metadata, Vector<uint8> data,
                                       const std::shared_ptr<std::atomic<bool>>& cancelRequested)
{
    const auto isCancelled = [&cancelRequested]
    { return cancelRequested && cancelRequested->load(std::memory_order_acquire); };

    // A cancel that lost the F12 arbitration (the job was already running or
    // about to run) still defuses the load here: skip the decode entirely.
    if (isCancelled())
    {
        return SharedPtr<Asset>{};
    }

    std::string assetLabel = GetAssetLogLabel(assetGuid, metadata.Path, &assetManager);

    SharedPtr<Asset> asset;
    {
        AssetDbProfiler::StageScope profileScope(AssetDbProfiler::Bucket::StageProcessing);
        const auto startTime = std::chrono::high_resolution_clock::now();

        try
        {
            // Establish the AssetManager context for any asset types that need to
            // resolve dependencies relative to the manager performing the load
            // (e.g., UI @import), and publish this load's cancel flag so
            // long-running decode work (the texture cook's BCn encode) can
            // abandon itself instead of running to completion after a cancel.
            AssetManager::ScopedThreadAssetManager ctx(&assetManager);
            ScopedAssetDecodeCancellation cancelCtx(cancelRequested);

            asset = ProcessAssetData(assetManager, assetGuid, metadata, data);

            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - startTime);
            Logger::Log::Debug("AssetDecodeJob: processed {} in {}μs", assetLabel, duration.count());
            AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
                "AssetProcessingTask", duration.count() / 1000.0, asset != nullptr);
        }
        // The decode reports a failure by returning no asset and never throws one: the web
        // build links without exception catching, so a throw aborts the module there and
        // every later load waits on this one forever. This catch stays for the decoders
        // that can still throw (a JSON or CSS parse, an allocation) on the builds that catch.
        catch (const std::exception& e)
        {
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - startTime);
            LogDecodeFailure(cancelRequested, "AssetDecodeJob: processing failed for {} after {}μs: {}",
                             assetLabel, duration.count(), e.what());
            AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
                "AssetProcessingTask", duration.count() / 1000.0, false);

            // Do not throw from worker threads; an empty result is a load failure.
            return SharedPtr<Asset>{};
        }
    }

    if (!asset)
    {
        return SharedPtr<Asset>{};
    }

    // Defuse before publishing: a cancelled load must never register its
    // asset — eject/unload/shutdown already decided it is unwanted, and a
    // late registration would leave a stale entry behind their cleanup.
    if (isCancelled())
    {
        return SharedPtr<Asset>{};
    }

    // Registration: store the loaded asset in the manager's cache (mutex-guarded
    // map insert with a <1ms target).
    {
        AssetDbProfiler::StageScope profileScope(AssetDbProfiler::Bucket::StageRegistration);
        const auto startTime = std::chrono::high_resolution_clock::now();

        assetManager.RegisterLoadedAsset(assetGuid, asset);

        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - startTime);
        const double durationMs = duration.count() / 1000.0;
        if (durationMs > 1.0)
        {
            Logger::Log::Warning("AssetDecodeJob: registration took {}ms, exceeding 1ms target for {}",
                                 durationMs, assetLabel);
        }
        AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
            "AssetRegistrationTask", durationMs, true);
    }

    return asset;
}

} // namespace

SharedPtr<Asset> DecodeAssetPayload(AssetManager& assetManager, const GUID& assetGuid,
                                    const AssetMetadata& metadata, const Vector<uint8>& data,
                                    const std::shared_ptr<std::atomic<bool>>& cancelRequested)
{
    try
    {
        // Same thread-local manager context the decode job establishes, so
        // asset types that resolve dependencies during load (UI @import,
        // humanoid auto-import) reach the manager performing the reload — plus
        // this reload generation's cancel flag, so a superseded or shut-down
        // reload stops its cook at the next block-row band boundary.
        AssetManager::ScopedThreadAssetManager ctx(&assetManager);
        ScopedAssetDecodeCancellation cancelCtx(cancelRequested);
        return ProcessAssetData(assetManager, assetGuid, metadata, data);
    }
    catch (const std::exception& e)
    {
        LogDecodeFailure(cancelRequested, "DecodeAssetPayload: decode failed for '{}': {}",
                         metadata.Path.string(), e.what());
        return SharedPtr<Asset>{};
    }
    catch (...)
    {
        LogDecodeFailure(cancelRequested, "DecodeAssetPayload: decode failed for '{}'",
                         metadata.Path.string());
        return SharedPtr<Asset>{};
    }
}

// ========================================
// UNIFIED ASSET UNLOAD TASK
// ========================================

AssetUnloadTask::AssetUnloadTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                 const GUID& assetGuid, AssetManager& assetManager)
    : JobSystem::Task(jobSystem), m_AssetGuid(assetGuid), m_AssetManager(assetManager)
{
}

void AssetUnloadTask::Execute()
{
    if (m_Cancelled.load())
    {
        throw std::runtime_error("Task was cancelled");
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    try
    {
        Logger::Log::Debug("AssetUnloadTask: Starting execution for asset {}", m_AssetGuid.ToString());

        // Unload asset from manager
        m_AssetManager.UnregisterLoadedAsset(m_AssetGuid);

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        Logger::Log::Debug("AssetUnloadTask: Completed for asset {} in {}μs",
                           m_AssetGuid.ToString(), duration.count());

        // Record performance metrics
        AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
            "AssetUnloadTask", duration.count() / 1000.0, true);

        // No explicit completion call; job system will mark the task as completed
    }
    catch (const std::exception& e)
    {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        Logger::Log::Error("AssetUnloadTask: Failed for asset {} after {}μs: {}",
                           m_AssetGuid.ToString(), duration.count(), e.what());

        // Record failure metrics
        AssetTaskPerformanceMonitor::GetInstance().RecordTaskExecution(
            "AssetUnloadTask", duration.count() / 1000.0, false);

        // Propagate the failure to the job system so it can mark the task as failed
        throw;
    }
}

// ========================================
// UNIFIED ASSET BATCH COORDINATOR
// ========================================

AssetBatchCoordinator::AssetBatchCoordinator(AssetManager& assetManager,
                                             JobSystem::WorkStealingThreadPool& jobSystem,
                                             AssetIOService& ioService)
    : m_AssetManager(assetManager), m_JobSystem(jobSystem), m_IOService(ioService)
{
}

void AssetBatchCoordinator::LoadAsset(const AssetMetadata& metadata, AssetLoadPriority priority,
                                      std::shared_ptr<std::atomic<bool>> cancelRequested,
                                      Function<void(const JobSystem::TaskHandle&)> onComplete,
                                      Function<void(const String&)> onFailure)
{
    const GUID assetGuid = metadata.Guid;

    // Withdraw before any work is queued: a load cancelled while it waited
    // for this pipeline build must not consume a read slot.
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire))
    {
        if (onFailure)
            onFailure("Asset load cancelled");
        return;
    }

    AssetIOService::ReadRequest request;
    request.AssetGuid = assetGuid;
    request.Metadata = metadata;
    request.Metadata.Path = m_AssetManager.GetParserRegistry().ResolveReadPath(metadata, m_AssetManager);
    request.Priority = priority;
    request.CancelRequested = cancelRequested;
    request.ProcessData = [manager = &m_AssetManager, assetGuid, metadata,
                           cancelRequested](Vector<uint8> data) -> SharedPtr<Asset>
    {
        return ExecuteAssetDecodeJob(*manager, assetGuid, metadata, std::move(data), cancelRequested);
    };
    request.OnSubmitted = [manager = &m_AssetManager, assetGuid,
                           onComplete = std::move(onComplete), onFailure](const JobSystem::TaskHandle& handle)
    {
        // Record the handle BEFORE wiring completion callbacks: the callbacks
        // erase the in-flight entry, so recording afterwards could write into
        // an entry for an already-resolved load.
        manager->SetInFlightDecodeHandle(assetGuid, handle);

        JobSystem::TaskHandle callbackHandle = handle;
        if (onComplete)
            callbackHandle.OnComplete(onComplete);
        if (onFailure)
            callbackHandle.OnFailure(onFailure);
    };
    request.OnFailure = std::move(onFailure);

    m_IOService.SubmitRead(std::move(request));
}

JobSystem::TaskHandle AssetBatchCoordinator::UnloadAsset(const GUID& assetGuid)
{
    return m_JobSystem.Submit(std::make_unique<AssetUnloadTask>(&m_JobSystem, assetGuid, m_AssetManager));
}

// ========================================
// ASSET TASK PERFORMANCE MONITOR
// ========================================

AssetTaskPerformanceMonitor& AssetTaskPerformanceMonitor::GetInstance()
{
    static AssetTaskPerformanceMonitor instance;
    return instance;
}

void AssetTaskPerformanceMonitor::RecordTaskExecution(const std::string& taskType, double executionTimeMs, bool success)
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    auto& stats = m_TaskStats[taskType];
    std::lock_guard<std::mutex> statsLock(stats.mutex);

    stats.executionTimes.push_back(executionTimeMs);
    if (!success)
    {
        stats.failureCount++;
    }

    // Keep only recent measurements (last 1000 executions)
    if (stats.executionTimes.size() > 1000)
    {
        stats.executionTimes.erase(stats.executionTimes.begin(),
                                   stats.executionTimes.begin() + 500);
    }
}

AssetTaskPerformanceMonitor::TaskMetrics AssetTaskPerformanceMonitor::GetTaskMetrics(const std::string& taskType) const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    auto it = m_TaskStats.find(taskType);
    if (it == m_TaskStats.end())
    {
        return TaskMetrics{0.0, 0.0, 0.0, 0, 0, 0.0};
    }

    const auto& stats = it->second;
    std::lock_guard<std::mutex> statsLock(stats.mutex);

    if (stats.executionTimes.empty())
    {
        return TaskMetrics{0.0, 0.0, 0.0, 0, stats.failureCount, 0.0};
    }

    TaskMetrics metrics;
    metrics.totalExecutions = stats.executionTimes.size();
    metrics.failureCount = stats.failureCount;

    double sum = 0.0;
    metrics.minExecutionTimeMs = stats.executionTimes[0];
    metrics.maxExecutionTimeMs = stats.executionTimes[0];

    for (double time : stats.executionTimes)
    {
        sum += time;
        metrics.minExecutionTimeMs = std::min(metrics.minExecutionTimeMs, time);
        metrics.maxExecutionTimeMs = std::max(metrics.maxExecutionTimeMs, time);
    }

    metrics.averageExecutionTimeMs = sum / stats.executionTimes.size();
    metrics.successRate = (double)(metrics.totalExecutions - metrics.failureCount) / metrics.totalExecutions;

    return metrics;
}

AssetTaskPerformanceMonitor::PipelineMetrics AssetTaskPerformanceMonitor::GetPipelineMetrics() const
{
    PipelineMetrics pipeline;
    pipeline.fileIOMetrics = GetTaskMetrics("AssetFileIOTask");

    pipeline.parsingMetrics = GetTaskMetrics("AssetProcessingTask");

    pipeline.registrationMetrics = GetTaskMetrics("AssetRegistrationTask");

    pipeline.totalPipelineTimeMs = pipeline.fileIOMetrics.averageExecutionTimeMs +
                                   pipeline.parsingMetrics.averageExecutionTimeMs +
                                   pipeline.registrationMetrics.averageExecutionTimeMs;

    // Estimate concurrent pipelines based on recent activity
    pipeline.concurrentPipelines = std::max({pipeline.fileIOMetrics.totalExecutions,
                                             pipeline.parsingMetrics.totalExecutions,
                                             pipeline.registrationMetrics.totalExecutions});

    return pipeline;
}

void AssetTaskPerformanceMonitor::ResetMetrics()
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    m_TaskStats.clear();
}

bool AssetTaskPerformanceMonitor::ValidatePerformanceTargets() const
{
    auto pipeline = GetPipelineMetrics();

    // Sanitizers (especially ASan) add measurable overhead; relax perf targets in those builds.
    double targetScale = 1.0;
#if defined(GE_ENABLE_ASAN) && GE_ENABLE_ASAN
    targetScale = 2.0;
#endif

    // Check performance targets for the load pipeline (FileIO read + Processing + Registration)
    bool fileIOTarget = pipeline.fileIOMetrics.averageExecutionTimeMs <= (10.0 * targetScale);            // 10ms target
    bool processingTarget = pipeline.parsingMetrics.averageExecutionTimeMs <= (50.0 * targetScale);       // 50ms target
    bool registrationTarget = pipeline.registrationMetrics.averageExecutionTimeMs <= (1.0 * targetScale); // 1ms target
    bool successRateTarget = pipeline.fileIOMetrics.successRate >= 0.99 || pipeline.fileIOMetrics.totalExecutions == 0;

    return fileIOTarget && processingTarget && registrationTarget && successRateTarget;
}

std::string AssetTaskPerformanceMonitor::GeneratePerformanceReport() const
{
    auto pipeline = GetPipelineMetrics();

    std::ostringstream report;
    report << "=== Asset Task Performance Report ===\n";
    report << "File I/O Task:\n";
    report << "  Average: " << pipeline.fileIOMetrics.averageExecutionTimeMs << "ms\n";
    report << "  Min/Max: " << pipeline.fileIOMetrics.minExecutionTimeMs << "/" << pipeline.fileIOMetrics.maxExecutionTimeMs << "ms\n";
    report << "  Success Rate: " << (pipeline.fileIOMetrics.successRate * 100.0) << "%\n";
    report << "  Total Executions: " << pipeline.fileIOMetrics.totalExecutions << "\n\n";

    report << "Processing Task:\n";
    report << "  Average: " << pipeline.parsingMetrics.averageExecutionTimeMs << "ms\n";
    report << "  Min/Max: " << pipeline.parsingMetrics.minExecutionTimeMs << "/" << pipeline.parsingMetrics.maxExecutionTimeMs << "ms\n";
    report << "  Success Rate: " << (pipeline.parsingMetrics.successRate * 100.0) << "%\n";
    report << "  Total Executions: " << pipeline.parsingMetrics.totalExecutions << "\n\n";

    report << "Registration Task:\n";
    report << "  Average: " << pipeline.registrationMetrics.averageExecutionTimeMs << "ms\n";
    report << "  Min/Max: " << pipeline.registrationMetrics.minExecutionTimeMs << "/" << pipeline.registrationMetrics.maxExecutionTimeMs << "ms\n";
    report << "  Success Rate: " << (pipeline.registrationMetrics.successRate * 100.0) << "%\n";
    report << "  Total Executions: " << pipeline.registrationMetrics.totalExecutions << "\n\n";

    report << "Pipeline Summary:\n";
    report << "  Total Pipeline Time: " << pipeline.totalPipelineTimeMs << "ms\n";
    report << "  Performance Targets Met: " << (ValidatePerformanceTargets() ? "YES" : "NO") << "\n";

    return report.str();
}

} // namespace GameEngine

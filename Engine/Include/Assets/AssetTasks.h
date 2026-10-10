#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "JobSystem/Task.h"
#include "Types/Types.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

// Use fully qualified JobSystem types to avoid conflicts

// Forward declarations
class AssetIOService;
class AssetManager;
class AssetRegistry;

/**
 * @brief Unified asset unload task for removing assets from memory
 *
 * Unloads an asset from the AssetManager.
 * No dependencies - can execute immediately.
 * Execution time: 1-5ms (cleanup operations)
 */
class AssetUnloadTask : public JobSystem::Task
{
  public:
    AssetUnloadTask(JobSystem::WorkStealingThreadPool* jobSystem,
                    const GUID& assetGuid, AssetManager& assetManager);

    void Execute() override;
    bool IsCancelled() const override { return m_Cancelled.load(); }
    void Cancel() { m_Cancelled.store(true); }

    const GUID& GetAssetGuid() const { return m_AssetGuid; }

  private:
    GUID m_AssetGuid;
    AssetManager& m_AssetManager;
    std::atomic<bool> m_Cancelled{false};
};

/**
 * @brief Builds the per-asset load pipeline: blocking read on the
 * AssetIOService, then one merged processing+registration job submitted to
 * the JobSystem at JobPriority::Background (read-then-submit — no
 * dependency-graph edges).
 */
class AssetBatchCoordinator
{
  public:
    AssetBatchCoordinator(AssetManager& assetManager, JobSystem::WorkStealingThreadPool& jobSystem,
                          AssetIOService& ioService);

    /**
     * @brief Kick off the read -> Background-decode pipeline for one asset.
     * The caller supplies the (already registry-validated) metadata; this
     * hand-off is a cancel check plus an AssetIOService queue push — cheap
     * and non-blocking, safe to call inline from the submitting thread.
     *
     * Exactly one of the callbacks eventually fires:
     * - onComplete with the decode job's terminal handle. The handle's result
     *   is the loaded asset, or null when processing failed.
     * - onFailure when no decode job could run (read failure, cancellation,
     *   AssetIOService stopped, JobSystem shutdown gate) or when the decode
     *   job was cancelled.
     *
     * cancelRequested is the in-flight entry's cancel flag (null = not
     * cancellable): a load whose flag is set is withdrawn at the earliest
     * checkpoint — before the read is queued, when a reader claims it, before
     * the decode is submitted, or inside the decode job before the asset is
     * registered.
     *
     * The decode job's TaskHandle is recorded on the AssetManager's in-flight
     * entry (the load's cancellation surface).
     */
    void LoadAsset(const AssetMetadata& metadata, AssetLoadPriority priority,
                   std::shared_ptr<std::atomic<bool>> cancelRequested,
                   Function<void(const JobSystem::TaskHandle&)> onComplete,
                   Function<void(const String&)> onFailure);

    /**
     * @brief Unload single asset
     * @param assetGuid Asset to unload
     * @return TaskHandle for tracking unload completion
     */
    JobSystem::TaskHandle UnloadAsset(const GUID& assetGuid);

  private:
    AssetManager& m_AssetManager;
    JobSystem::WorkStealingThreadPool& m_JobSystem;
    AssetIOService& m_IOService;
};

/**
 * @brief Decode raw file bytes into a fresh, UNREGISTERED asset instance
 * (create + LoadFromData/type-specific load + PostLoad) — the same
 * type-routing the initial-load decode job uses, minus the loaded-map
 * registration. The async reload pipeline runs this on a worker to stage the
 * changed file's payload, then adopts it into the live asset on the main
 * thread. Returns null on decode failure; never throws.
 *
 * `cancelRequested` is the reload generation's flag, published for the decode's
 * call tree (see ScopedAssetDecodeCancellation) so long-running work such as the
 * texture cook abandons itself when the reload is superseded or shut down.
 */
SharedPtr<Asset> DecodeAssetPayload(AssetManager& assetManager, const GUID& assetGuid,
                                    const AssetMetadata& metadata, const Vector<uint8>& data,
                                    const std::shared_ptr<std::atomic<bool>>& cancelRequested = {});

/**
 * @brief Asset task performance monitor for optimization and debugging
 *
 * Tracks performance metrics for asset loading operations.
 */
class AssetTaskPerformanceMonitor
{
  public:
    struct TaskMetrics
    {
        double averageExecutionTimeMs;
        double maxExecutionTimeMs;
        double minExecutionTimeMs;
        size_t totalExecutions;
        size_t failureCount;
        double successRate;
    };

    struct PipelineMetrics
    {
        TaskMetrics fileIOMetrics;
        TaskMetrics parsingMetrics;
        TaskMetrics registrationMetrics;
        double totalPipelineTimeMs;
        size_t concurrentPipelines;
    };

    static AssetTaskPerformanceMonitor& GetInstance();

    void RecordTaskExecution(const std::string& taskType, double executionTimeMs, bool success);
    TaskMetrics GetTaskMetrics(const std::string& taskType) const;
    PipelineMetrics GetPipelineMetrics() const;
    void ResetMetrics();

    // Performance validation
    bool ValidatePerformanceTargets() const;
    std::string GeneratePerformanceReport() const;

  private:
    AssetTaskPerformanceMonitor() = default;

    struct TaskStats
    {
        std::vector<double> executionTimes;
        size_t failureCount{0};
        mutable std::mutex mutex;
    };

    std::unordered_map<std::string, TaskStats> m_TaskStats;
    mutable std::mutex m_StatsMutex;
};

} // namespace GameEngine

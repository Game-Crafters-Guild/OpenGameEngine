#pragma once

#include "Assets/AssetRegistry.h"
#include "JobSystem/TaskTypes.h"
#include "JobSystem/Task.h"
#include "Types/Types.h"
#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace GameEngine
{

// Use fully qualified JobSystem types to avoid conflicts

// Forward declarations
class AssetRegistry;

/**
 * @brief The error a scan's future carries when AsyncRegistryCoordinator::CancelScans stopped
 * the scan before it finished. Batches registered before the stop stay registered; the
 * post-scan reconcile does not run.
 */
class AssetScanCancelled final : public std::runtime_error
{
  public:
    AssetScanCancelled() : std::runtime_error("asset scan cancelled") {}
};

/**
 * Cached stats captured during directory iteration. Populated from the
 * directory_entry while the iterator is still hot — avoids the extra
 * stat syscalls that would otherwise fire when the metadata pipeline
 * later asks for last_write_time / file_size on each path. Phase 5.1
 * "stat collapse" optimization.
 *
 * If hasCachedStats is false the consumer falls back to re-stat'ing the
 * path (used by the BatchProcessAssetsAsync path where callers hand us
 * raw paths without going through DirectoryScanTask).
 */
struct ScannedFileInfo
{
    std::filesystem::path path;
    std::filesystem::file_time_type lastModified{};
    uintmax_t fileSize = 0;
    bool hasCachedStats = false;
};

/**
 * @brief Directory Scan Task - Recursively scans directories for assets
 * Runs on background thread, 10-100ms execution time
 */
class DirectoryScanTask : public JobSystem::Task
{
  public:
    DirectoryScanTask(JobSystem::WorkStealingThreadPool* jobSystem,
                      const std::filesystem::path& directory,
                      bool recursive,
                      const AssetIgnoreRules& ignoreRules,
                      std::promise<Vector<ScannedFileInfo>> foundAssetsPromise,
                      std::shared_ptr<const std::atomic<bool>> cancelRequested,
                      std::unordered_map<std::string, int64_t> snapshotDirMtimes = {});

    void Execute() override;
    bool IsCancelled() const override { return m_CancelRequested->load(); }

  private:
    std::filesystem::path m_Directory;
    bool m_Recursive;
    AssetIgnoreRules m_IgnoreRules;
    std::promise<Vector<ScannedFileInfo>> m_FoundAssetsPromise;
    std::shared_ptr<const std::atomic<bool>> m_CancelRequested;

    // C.2: per-subdirectory mtime map from the warm-start snapshot.
    // When a directory_iterator visits a subdir whose canonical-rel
    // path is in this map AND its current mtime matches, the subtree
    // recursion is disabled — its files were proven snapshot-current
    // at the previous Shutdown and are already in the registry maps
    // from PopulateHotCachesFromSource.
    std::unordered_map<std::string, int64_t> m_SnapshotDirMtimes;

    // Helper methods
    void ScanDirectoryRecursive(const std::filesystem::path& dir, Vector<ScannedFileInfo>& assets);
    bool ShouldIncludeFile(const std::filesystem::path& filePath) const;
};

/**
 * @brief Metadata Batch Processing Task - Processes multiple asset metadata files
 * Runs on worker thread, 5-20ms execution time
 */
class MetadataBatchProcessingTask : public JobSystem::Task
{
  public:
    MetadataBatchProcessingTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                std::future<Vector<ScannedFileInfo>> assetInfosFuture,
                                AssetRegistry& registry,
                                std::promise<Vector<AssetMetadata>> processedMetadataPromise,
                                std::filesystem::path mountRoot,
                                std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint> snapshotByPath,
                                std::shared_ptr<const std::atomic<bool>> cancelRequested);

    void Execute() override;
    bool IsCancelled() const override { return m_CancelRequested->load(); }

  private:
    bool ProcessChunk(const Vector<ScannedFileInfo>& assetInfos, size_t chunk, Vector<AssetMetadata>& out) const;

    std::future<Vector<ScannedFileInfo>> m_AssetInfosFuture;
    AssetRegistry& m_Registry;
    std::promise<Vector<AssetMetadata>> m_ProcessedMetadataPromise;
    std::shared_ptr<const std::atomic<bool>> m_CancelRequested;

    // B.3b: warm-start snapshot data so the worker pool can skip
    // ProcessSingleAssetForScan + RegistryUpdateTask for files whose
    // (mtime, size) match the snapshot — those entries are already in
    // the registry maps from PopulateHotCachesFromSource at Initialize.
    std::filesystem::path m_MountRoot;
    std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint> m_SnapshotByPath;
};

/**
 * @brief Registry Update Task - Updates registry with processed metadata
 * Runs on main thread, 1-2ms execution time
 */
class RegistryUpdateTask : public JobSystem::Task
{
  public:
    // scanRoot: the directory this scan covered. After the final register
    // batch commits (and before the count promise resolves) the task hands
    // it to AssetRegistry::ReconcileDerivedSourceAfterScan, along with the
    // batches' !hadExisting registrations as rename-heal candidates — a
    // full-mount scan matches its source's root and reconciles; partial
    // scans match nothing. A cancelled scan skips the reconcile.
    RegistryUpdateTask(JobSystem::WorkStealingThreadPool* jobSystem,
                       std::future<Vector<AssetMetadata>> metadataFuture,
                       AssetRegistry& registry,
                       std::promise<size_t> registeredCountPromise,
                       std::filesystem::path scanRoot,
                       std::shared_ptr<const std::atomic<bool>> cancelRequested);

    void Execute() override;
    bool IsCancelled() const override { return m_CancelRequested->load(); }

  private:
    bool RegisterChunk(Vector<AssetMetadata>& metadataList, size_t chunk,
                       std::vector<AssetDatabase::ReconcileScanNewFile>& newFiles,
                       std::atomic<size_t>& registeredCount);

    std::future<Vector<AssetMetadata>> m_MetadataFuture;
    AssetRegistry& m_Registry;
    std::promise<size_t> m_RegisteredCountPromise;
    std::filesystem::path m_ScanRoot;
    std::shared_ptr<const std::atomic<bool>> m_CancelRequested;
};

/**
 * @brief Asset Registry Pipeline Coordinator - Manages async registry operations
 */
class AsyncRegistryCoordinator
{
  public:
    AsyncRegistryCoordinator(AssetRegistry& registry, JobSystem::WorkStealingThreadPool& jobSystem);

    /**
     * @brief Start async directory scanning
     */
    std::future<size_t> ScanDirectoryAsync(const std::filesystem::path& directory, bool recursive = true);

    /**
     * @brief Stop every scan this coordinator started. Each stage stops at its next file or chunk
     * boundary, waits for the stage before it, and fails its future with AssetScanCancelled, so a
     * scan's future still resolves only after all of the scan's tasks are done with the registry.
     * Irreversible: a scan started afterwards stops at once.
     */
    void CancelScans();

    /**
     * @brief Start incremental scan for changes
     */
    std::future<size_t> IncrementalScanAsync(const std::filesystem::path& directory,
                                             std::chrono::file_clock::time_point lastScanTime);

    /**
     * @brief Batch process multiple assets
     */
    std::future<size_t> BatchProcessAssetsAsync(const Vector<std::filesystem::path>& assetPaths);

  private:
    AssetRegistry& m_Registry;
    JobSystem::WorkStealingThreadPool& m_JobSystem;
    // Shared by every task of every scan this coordinator starts; the tasks' references keep it
    // alive past the coordinator.
    std::shared_ptr<std::atomic<bool>> m_CancelRequested = std::make_shared<std::atomic<bool>>(false);

    // Helper methods
    void SubmitDirectoryScanTask(const std::filesystem::path& directory, bool recursive,
                                 std::promise<Vector<ScannedFileInfo>> foundAssetsPromise);
    void SubmitMetadataProcessingTask(const Vector<ScannedFileInfo>& assetInfos,
                                      std::promise<Vector<AssetMetadata>> processedMetadataPromise);
    void SubmitRegistryUpdateTask(std::future<Vector<AssetMetadata>> metadataFuture,
                                  std::promise<size_t> registeredCountPromise,
                                  const std::filesystem::path& scanRoot);
};

/**
 * @brief Registry Performance Monitor - Tracks async operation performance
 */
struct RegistryPerformanceStats
{
    size_t totalFilesScanned{0};
    size_t totalAssetsRegistered{0};
    uint64_t averageScanTimeMicros{0};
    uint64_t averageMetadataProcessingTimeMicros{0};
    uint64_t totalScanTimeMicros{0};
    std::chrono::time_point<std::chrono::high_resolution_clock> lastScanTime;
};

class RegistryPerformanceMonitor
{
  public:
    void RecordScanOperation(size_t filesScanned, uint64_t timeMicros);
    void RecordMetadataOperation(size_t assetsProcessed, uint64_t timeMicros);
    const RegistryPerformanceStats& GetStats() const { return m_Stats; }
    void Reset();

  private:
    RegistryPerformanceStats m_Stats;
    std::mutex m_StatsMutex;
};

} // namespace GameEngine

#include "Assets/AsyncRegistryTasks.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/PathNormalization.h"
#include "AssetDatabase/AssetStoreReconciler.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_set>

namespace GameEngine
{

namespace
{
static bool ShouldExtractDependenciesDuringScan(const AssetMetadata& /*md*/)
{
    // Dependency extraction reads every text file from disk + regex-scans for GUIDs/paths.
    // This adds ~6 seconds to startup for the editor asset source alone (CSS files with
    // @import dependencies scanned twice — once per source). Since dependency data is only
    // used for reference queries ("Find References"), not for loading, defer it entirely.
    // The dependency graph can be built lazily on first query instead.
    return false;
}

static std::exception_ptr ScanCancelledError()
{
    return std::make_exception_ptr(AssetScanCancelled());
}

static AssetMetadata ProcessSingleAssetForScan(AssetRegistry& registry, const ScannedFileInfo& scanInfo)
{
    const std::filesystem::path& assetPath = scanInfo.path;
    AssetMetadata metadata;
    metadata.Path = assetPath;
    metadata.Name = assetPath.stem().string();
    metadata.Extension = GetCompoundExtensionFromPath(assetPath.string());

    // Phase 5.1 stat collapse: prefer pre-fetched stats from the directory
    // iterator when available. Falls back to a fresh stat for paths handed
    // in directly by BatchProcessAssetsAsync (no iterator context).
    if (scanInfo.hasCachedStats)
    {
        metadata.LastModified = scanInfo.lastModified;
        metadata.FileSize = static_cast<size_t>(scanInfo.fileSize);
    }
    else
    {
        // Use error_code variants to avoid exceptions during async scanning
        // (files may change mid-scan).
        std::error_code ec;
        metadata.LastModified = std::filesystem::last_write_time(assetPath, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to get last_write_time for {}: {}", assetPath.string(), ec.message());
            metadata.Path.clear();
            return metadata;
        }
        ec.clear();
        metadata.FileSize = static_cast<size_t>(std::filesystem::file_size(assetPath, ec));
        if (ec)
        {
            Logger::Log::Warning("Failed to get file_size for {}: {}", assetPath.string(), ec.message());
            metadata.Path.clear();
            return metadata;
        }
    }

    // Determine asset type (parser sniffing when available, otherwise extension mapping).
    // An extension the engine cannot classify leaves TypeId empty rather than
    // spelling out "Unknown": downstream merges read a type id as an answer, and
    // a manufactured one overwrites the answer a caller supplied.
    metadata.Type = registry.ClassifyAssetType(assetPath);
    if (metadata.Type != AssetType::Unknown)
    {
        metadata.TypeId = AssetTypeToString(metadata.Type);
    }
    metadata.Guid = GUID::Null(); // identity comes from AssetRegistry/authoritative store

    // Derived dependency extraction (cache-backed).
    if (ShouldExtractDependenciesDuringScan(metadata))
    {
        AssetDependencyInfo info = AssetDependencyExtractor::ExtractDependencies(metadata);
        AssetDependencyExtractor::ResolveDependencyPathsToGuids(info, metadata, registry);
        metadata.Dependencies = info.Dependencies;
        metadata.DependencyPaths.clear();
        metadata.DependencyPaths.reserve(info.DependencyPaths.size());
        for (const auto& p : info.DependencyPaths)
        {
            metadata.DependencyPaths.push_back(p);
        }
        metadata.DependenciesExtracted = true;
    }

    return metadata;
}

struct MetadataFanoutState
{
    std::mutex mutex;
    Vector<ScannedFileInfo> assetInfos;
    std::atomic<size_t> nextIndex{0};
    Vector<AssetMetadata> combined;
    std::atomic<size_t> activeWorkers{0};
    std::atomic<bool> done{false};
    std::chrono::high_resolution_clock::time_point startTime{};
    std::promise<Vector<AssetMetadata>> promise;
    // B.3b: warm-start snapshot threaded through so workers can skip
    // ProcessSingleAssetForScan + downstream RegistryUpdateTask work
    // for files whose (mtime, size) match the snapshot.
    std::filesystem::path mountRoot;
    std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint> snapshotByPath;
    std::shared_ptr<const std::atomic<bool>> cancelRequested;
};

// B.3b: shared between the parallel worker pool and the synchronous fallback
// loop. Returns true if this scanned file is provably unchanged since the
// snapshot was written, so the registry already has it from
// PopulateHotCachesFromSource and the per-file scan pipeline can be skipped.
static bool IsScannedFileSnapshotCurrent(
    const std::filesystem::path& mountRoot,
    const std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint>& snapshotByPath,
    const ScannedFileInfo& info)
{
    if (snapshotByPath.empty() || !info.hasCachedStats || mountRoot.empty())
        return false;

    std::string canonicalRel;
    if (!AssetRegistry::TryComputeCanonicalRelativePath(mountRoot, info.path, canonicalRel))
        return false;

    auto it = snapshotByPath.find(AssetPaths::FoldStorePathKey(canonicalRel));
    if (it == snapshotByPath.end())
        return false;

    const auto& snap = it->second;
    const int64_t scanMtime = static_cast<int64_t>(info.lastModified.time_since_epoch().count());
    return snap.Mtime == scanMtime &&
           snap.Size == static_cast<int64_t>(info.fileSize);
}

class MetadataWorkerTask final : public JobSystem::Task
{
  public:
    MetadataWorkerTask(JobSystem::WorkStealingThreadPool* jobSystem,
                       AssetRegistry& registry,
                       std::shared_ptr<MetadataFanoutState> state,
                       size_t chunkSize)
        : JobSystem::Task(jobSystem), m_Registry(registry), m_State(std::move(state)), m_ChunkSize(chunkSize)
    {
    }

    void Execute() override
    {
        if (!m_State)
            return;

        Vector<AssetMetadata> local;
        while (!m_State->cancelRequested->load())
        {
            const size_t start = m_State->nextIndex.fetch_add(m_ChunkSize, std::memory_order_relaxed);
            if (start >= m_State->assetInfos.size())
                break;

            const size_t end = std::min(start + m_ChunkSize, m_State->assetInfos.size());
            local.reserve(local.size() + (end - start));

            for (size_t i = start; i < end && !m_State->cancelRequested->load(); ++i)
            {
                const auto& info = m_State->assetInfos[i];
                // Existence check is only needed for un-cached entries; cached
                // entries came from the iterator and were live at scan time
                // (the small remaining race is acceptable for non-tombstoned
                // metadata that the registry will reconcile later).
                if (!info.hasCachedStats)
                {
                    std::error_code ec;
                    if (!std::filesystem::exists(info.path, ec))
                        continue;
                }

                // B.3b: snapshot-current files are already in the registry
                // (populated by PopulateHotCachesFromSource at Initialize).
                // Skip ClassifyAssetType + dep extract + downstream registry
                // update — there's nothing to do for them.
                if (IsScannedFileSnapshotCurrent(m_State->mountRoot, m_State->snapshotByPath, info))
                {
                    continue;
                }

                AssetMetadata md = ProcessSingleAssetForScan(m_Registry, info);
                if (!md.Path.empty())
                {
                    local.push_back(std::move(md));
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_State->mutex);
            for (auto& md : local)
            {
                m_State->combined.push_back(std::move(md));
            }
        }

        const size_t prev = m_State->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1 && !m_State->done.exchange(true))
        {
            // Last worker finishes the batch and fulfills the promise. A worker
            // that stopped early for cancellation left its chunks unprocessed,
            // so a cancelled batch is never handed on as complete.
            if (m_State->cancelRequested->load())
            {
                m_State->promise.set_exception(ScanCancelledError());
                return;
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - m_State->startTime);

            Logger::Log::Debug("MetadataBatchProcessingTask: Processed {} assets in {}μs",
                               m_State->combined.size(), duration.count());

            m_State->promise.set_value(std::move(m_State->combined));
        }
    }

  private:
    AssetRegistry& m_Registry;
    std::shared_ptr<MetadataFanoutState> m_State;
    size_t m_ChunkSize = 256;
};

// E2: shared state for the parallel register fan-in. Mirrors
// MetadataFanoutState — workers pull index ranges, resolve GUIDs, and hand
// each chunk to AssetRegistry::RegisterAssetMetadataBatch (one writer-lock
// scope + one derived-cache transaction per chunk). The last worker
// fulfills the promise with the summed register count.
struct RegisterFanoutState
{
    Vector<AssetMetadata> metadataList;
    std::atomic<size_t> nextIndex{0};
    std::atomic<size_t> registeredCount{0};
    std::atomic<size_t> activeWorkers{0};
    std::atomic<bool> done{false};
    std::promise<size_t> promise;
    // Scanned directory, forwarded so the last worker can run the derived
    // post-scan reconcile before it fulfills the promise.
    std::filesystem::path scanRoot;
    // Derived rename-heal candidates (the scan's !hadExisting registrations),
    // merged from every worker's batches; consumed by the last worker's
    // reconcile call. activeWorkers' acq_rel fence orders the merge before
    // the read, the mutex serializes concurrent merges.
    std::mutex newFilesMutex;
    std::vector<AssetDatabase::ReconcileScanNewFile> derivedNewFiles;
    std::shared_ptr<const std::atomic<bool>> cancelRequested;
};

class RegisterWorkerTask final : public JobSystem::Task
{
  public:
    RegisterWorkerTask(JobSystem::WorkStealingThreadPool* jobSystem,
                       AssetRegistry& registry,
                       std::shared_ptr<RegisterFanoutState> state,
                       size_t chunkSize)
        : JobSystem::Task(jobSystem), m_Registry(registry), m_State(std::move(state)), m_ChunkSize(chunkSize)
    {
    }

    void Execute() override
    {
        if (!m_State)
            return;

        std::vector<AssetDatabase::ReconcileScanNewFile> localNewFiles;
        // Cancellation is checked between chunks: a chunk resolves its GUIDs
        // and registers them as one unit.
        while (!m_State->cancelRequested->load())
        {
            const size_t start = m_State->nextIndex.fetch_add(m_ChunkSize, std::memory_order_relaxed);
            if (start >= m_State->metadataList.size())
                break;
            const size_t end = std::min(start + m_ChunkSize, m_State->metadataList.size());

            Vector<AssetMetadata> chunk;
            chunk.reserve(end - start);
            for (size_t i = start; i < end; ++i)
            {
                AssetMetadata& md = m_State->metadataList[i];
                if (md.Path.empty())
                    continue;
                // Resolve identity without expensive per-file work (mirrors
                // the serial fan-in).
                md.Guid = m_Registry.GetOrCreateAssetGUID(md.Path);
                if (md.Guid.IsNull())
                    continue;
                chunk.push_back(std::move(md));
            }
            if (!chunk.empty())
            {
                const size_t registered =
                    m_Registry.RegisterAssetMetadataBatch(std::move(chunk), &localNewFiles);
                m_State->registeredCount.fetch_add(registered, std::memory_order_relaxed);
            }
        }

        if (!localNewFiles.empty())
        {
            std::lock_guard<std::mutex> lk(m_State->newFilesMutex);
            m_State->derivedNewFiles.insert(m_State->derivedNewFiles.end(),
                                            std::make_move_iterator(localNewFiles.begin()),
                                            std::make_move_iterator(localNewFiles.end()));
        }

        const size_t prev = m_State->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1 && !m_State->done.exchange(true))
        {
            // A cancelled scan registered only some chunks. The reconcile
            // takes its rename-heal candidates from the registered files, so
            // on a partial scan it would miss heals and age ghosts a full scan
            // heals; a cancelled scan skips it.
            if (m_State->cancelRequested->load())
            {
                m_State->promise.set_exception(ScanCancelledError());
                return;
            }

            // Derived post-scan reconcile runs after the last register batch
            // and before the promise resolves, so StartupScanFuture waiters
            // observe a fully reconciled registry.
            m_Registry.ReconcileDerivedSourceAfterScan(
                m_State->scanRoot,
                m_State->derivedNewFiles.empty() ? nullptr : &m_State->derivedNewFiles);

            Logger::Log::Debug("RegisterWorkerTask: registered {} assets",
                               m_State->registeredCount.load(std::memory_order_relaxed));
            m_State->promise.set_value(m_State->registeredCount.load(std::memory_order_relaxed));
        }
    }

  private:
    AssetRegistry& m_Registry;
    std::shared_ptr<RegisterFanoutState> m_State;
    size_t m_ChunkSize = 256;
};

} // namespace

// DirectoryScanTask Implementation
DirectoryScanTask::DirectoryScanTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                     const std::filesystem::path& directory,
                                     bool recursive,
                                     const AssetIgnoreRules& ignoreRules,
                                     std::promise<Vector<ScannedFileInfo>> foundAssetsPromise,
                                     std::shared_ptr<const std::atomic<bool>> cancelRequested,
                                     std::unordered_map<std::string, int64_t> snapshotDirMtimes)
    : JobSystem::Task(jobSystem),
      m_Directory(directory),
      m_Recursive(recursive),
      m_IgnoreRules(ignoreRules),
      m_FoundAssetsPromise(std::move(foundAssetsPromise)),
      m_CancelRequested(std::move(cancelRequested)),
      m_SnapshotDirMtimes(std::move(snapshotDirMtimes))
{
}

void DirectoryScanTask::Execute()
{
    if (IsCancelled())
    {
        m_FoundAssetsPromise.set_exception(ScanCancelledError());
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    Vector<ScannedFileInfo> foundAssets;

    try
    {
        Logger::Log::Debug("DirectoryScanTask: Scanning directory {} (recursive: {})",
                           m_Directory.string(), m_Recursive);

        if (m_Recursive)
        {
            ScanDirectoryRecursive(m_Directory, foundAssets);
        }
        else
        {
            for (const auto& entry : std::filesystem::directory_iterator(m_Directory))
            {
                if (IsCancelled())
                    break;

                if (entry.is_regular_file() && ShouldIncludeFile(entry.path()))
                {
                    // Phase 5.1 stat collapse: capture mtime + size while the
                    // entry is still hot. directory_entry caches these on
                    // platforms that pre-populate them (Windows always; some
                    // POSIX filesystems on first access). On platforms that
                    // don't cache, this still runs the same syscall the
                    // metadata pipeline would have run later — net zero loss.
                    ScannedFileInfo info;
                    info.path = entry.path();
                    std::error_code ec;
                    info.lastModified = entry.last_write_time(ec);
                    if (!ec)
                    {
                        info.fileSize = entry.file_size(ec);
                        if (!ec)
                            info.hasCachedStats = true;
                    }
                    foundAssets.push_back(std::move(info));
                }
            }
        }

        // A walk stopped by cancellation found only part of the tree.
        if (IsCancelled())
        {
            m_FoundAssetsPromise.set_exception(ScanCancelledError());
            return;
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        Logger::Log::Debug("DirectoryScanTask: Found {} assets in {}μs",
                           foundAssets.size(), duration.count());

        m_FoundAssetsPromise.set_value(std::move(foundAssets));
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Error("DirectoryScanTask failed: {}", e.what());
        m_FoundAssetsPromise.set_exception(std::current_exception());
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("DirectoryScanTask failed: {}", e.what());
        m_FoundAssetsPromise.set_exception(std::current_exception());
    }
}

void DirectoryScanTask::ScanDirectoryRecursive(const std::filesystem::path& dir, Vector<ScannedFileInfo>& assets)
{
    // C.2 (post-audit fix): dir mtime does NOT propagate up the tree on
    // NTFS/POSIX. If we add a file to `models/characters/sub/` that bumps
    // sub's mtime but leaves models/characters/ and models/ unchanged.
    // The previous "if dir's mtime matches snapshot, disable recursion"
    // would skip models/ → never visit sub/ → miss the new file.
    //
    // Fix: pre-compute the set of subtrees that contain ANY dirty
    // descendant by stat'ing each snapshot dir up front and propagating
    // dirtiness UP the canonical-rel ancestry. A subtree is safe to skip
    // only when the dir AND none of its descendants is dirty.
    std::unordered_set<std::string> dirtyAncestors;
    if (!m_SnapshotDirMtimes.empty())
    {
        for (const auto& [dirRel, snapMtime] : m_SnapshotDirMtimes)
        {
            const std::filesystem::path absDir = m_Directory / std::filesystem::path(dirRel);
            std::error_code mtEc;
            const auto curMtime = std::filesystem::last_write_time(absDir, mtEc);
            const bool dirtyByStat = mtEc ||
                static_cast<int64_t>(curMtime.time_since_epoch().count()) != snapMtime;
            if (!dirtyByStat)
                continue;
            // Mark this dir + every ancestor up to root.
            std::filesystem::path p(dirRel);
            while (true)
            {
                std::string s = p.generic_string();
                if (s.empty() || s == ".")
                    break;
                if (!dirtyAncestors.insert(s).second)
                    break;  // already marked — its ancestors are too.
                if (!p.has_parent_path() || p.parent_path() == p)
                    break;
                p = p.parent_path();
            }
        }
    }

    try
    {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (IsCancelled())
            {
                return;
            }

            if (ec)
            {
                ec.clear();
                continue;
            }

            const auto& entry = *it;
            const auto p = entry.path();

            if (entry.is_directory(ec))
            {
                if (m_IgnoreRules.ShouldIgnoreDirectory(p, m_Directory))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                // C.2: subtree-skip when the snapshot covers this dir AND
                // its precomputed dirty-ancestor closure says no descendant
                // changed. Files inside an unchanged subtree were proven
                // snapshot-current at the previous Shutdown and are already
                // in the registry maps from PopulateHotCachesFromSource.
                //
                // Tradeoff: between-session file-CONTENT edits (which don't
                // bump dir mtime) go undetected here — file watchers +
                // load-time validation are the recovery path.
                if (!m_SnapshotDirMtimes.empty())
                {
                    std::string canonicalRel;
                    if (AssetRegistry::TryComputeCanonicalRelativePath(m_Directory, p, canonicalRel))
                    {
                        // Skip this dir's subtree only if (a) the snapshot
                        // recorded it, and (b) NEITHER it nor any descendant
                        // is in the dirty-ancestor closure.
                        if (m_SnapshotDirMtimes.find(canonicalRel) != m_SnapshotDirMtimes.end() &&
                            dirtyAncestors.find(canonicalRel) == dirtyAncestors.end())
                        {
                            it.disable_recursion_pending();
                            continue;
                        }
                    }
                }
                continue;
            }

            if (entry.is_regular_file(ec) && ShouldIncludeFile(p))
            {
                // Phase 5.1 stat collapse: capture mtime + size while the
                // directory_entry is still hot. On platforms that prefetch
                // (Windows recursive_directory_iterator always; some POSIX
                // filesystems opportunistically) these calls are cache hits.
                ScannedFileInfo info;
                info.path = p;
                std::error_code ec2;
                info.lastModified = entry.last_write_time(ec2);
                if (!ec2)
                {
                    info.fileSize = entry.file_size(ec2);
                    if (!ec2)
                        info.hasCachedStats = true;
                }
                assets.push_back(std::move(info));
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Warning("Error scanning subdirectory {}: {}", dir.string(), e.what());
    }
}

bool DirectoryScanTask::ShouldIncludeFile(const std::filesystem::path& filePath) const
{
    // Rooted ignore rules (defaults + optional <AssetRoot>/.assetignore).
    // Note: We treat the scan directory as the asset root for these checks.
    return !m_IgnoreRules.ShouldIgnoreFile(filePath, m_Directory);
}

// MetadataBatchProcessingTask Implementation
MetadataBatchProcessingTask::MetadataBatchProcessingTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                                         std::future<Vector<ScannedFileInfo>> assetInfosFuture,
                                                         AssetRegistry& registry,
                                                         std::promise<Vector<AssetMetadata>> processedMetadataPromise,
                                                         std::filesystem::path mountRoot,
                                                         std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint> snapshotByPath,
                                                         std::shared_ptr<const std::atomic<bool>> cancelRequested)
    : JobSystem::Task(jobSystem),
      m_AssetInfosFuture(std::move(assetInfosFuture)),
      m_Registry(registry),
      m_ProcessedMetadataPromise(std::move(processedMetadataPromise)),
      m_CancelRequested(std::move(cancelRequested)),
      m_MountRoot(std::move(mountRoot)),
      m_SnapshotByPath(std::move(snapshotByPath))
{
}

void MetadataBatchProcessingTask::Execute()
{
    auto startTime = std::chrono::high_resolution_clock::now();
    Vector<AssetMetadata> processedMetadata;
    Vector<ScannedFileInfo> assetInfos;

    try
    {
        // Resolve the upstream file list (may block this worker thread, but will not block the caller thread).
        // Cancellation is checked only after it resolves, so this stage never finishes while the walk
        // before it still runs.
        assetInfos = m_AssetInfosFuture.get();

        if (IsCancelled())
        {
            m_ProcessedMetadataPromise.set_exception(ScanCancelledError());
            return;
        }

        Logger::Log::Debug("MetadataBatchProcessingTask: Processing {} assets", assetInfos.size());

        if (assetInfos.empty())
        {
            m_ProcessedMetadataPromise.set_value({});
            return;
        }

        // Parallelize metadata extraction for large scans.
        // NOTE: This is derived data; prioritize responsiveness over fully saturating the job system.
        constexpr size_t kChunkSize = 256;
        const size_t chunkCount = (assetInfos.size() + (kChunkSize - 1)) / kChunkSize;

        JobSystem::WorkStealingThreadPool* js = GetJobSystem();
        if (js && chunkCount > 1)
        {
            auto state = std::make_shared<MetadataFanoutState>();
            state->assetInfos = std::move(assetInfos);
            state->combined.reserve(state->assetInfos.size());
            state->startTime = startTime;
            state->promise = std::move(m_ProcessedMetadataPromise);
            state->mountRoot = m_MountRoot;
            state->snapshotByPath = std::move(m_SnapshotByPath);
            state->cancelRequested = m_CancelRequested;

            // Throttle concurrency: use only a subset of the pool so other async jobs still make progress.
            const size_t workerCount = js->GetWorkerCount();
            const size_t maxScanWorkers = std::max<size_t>(1, workerCount > 1 ? (workerCount / 2) : 1);
            const size_t workersToSpawn = std::min(maxScanWorkers, chunkCount);
            state->activeWorkers.store(workersToSpawn, std::memory_order_relaxed);

            for (size_t i = 0; i < workersToSpawn; ++i)
            {
                auto task = std::make_unique<MetadataWorkerTask>(js, m_Registry, state, kChunkSize);
                js->Submit(std::move(task));
            }

            // IMPORTANT: Do not fulfill m_ProcessedMetadataPromise here; the last worker will.
            return;
        }

        processedMetadata.reserve(assetInfos.size());

        for (const auto& info : assetInfos)
        {
            if (IsCancelled())
            {
                m_ProcessedMetadataPromise.set_exception(ScanCancelledError());
                return;
            }

            // Cached entries skip the existence check (the scan saw it live);
            // un-cached entries (BatchProcessAssetsAsync raw paths) re-check.
            if (!info.hasCachedStats)
            {
                std::error_code ec;
                if (!std::filesystem::exists(info.path, ec))
                    continue;
            }

            // B.3b: snapshot-current files already populated by
            // PopulateHotCachesFromSource at Initialize — skip downstream work.
            if (IsScannedFileSnapshotCurrent(m_MountRoot, m_SnapshotByPath, info))
                continue;

            AssetMetadata metadata = ProcessSingleAssetForScan(m_Registry, info);
            if (!metadata.Path.empty())
            {
                processedMetadata.push_back(std::move(metadata));
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        Logger::Log::Debug("MetadataBatchProcessingTask: Processed {} assets in {}μs",
                           processedMetadata.size(), duration.count());

        m_ProcessedMetadataPromise.set_value(std::move(processedMetadata));
    }
    catch (const AssetScanCancelled&)
    {
        m_ProcessedMetadataPromise.set_exception(std::current_exception());
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("MetadataBatchProcessingTask failed: {}", e.what());
        m_ProcessedMetadataPromise.set_exception(std::current_exception());
    }
}

// RegistryUpdateTask Implementation
RegistryUpdateTask::RegistryUpdateTask(JobSystem::WorkStealingThreadPool* jobSystem,
                                       std::future<Vector<AssetMetadata>> metadataFuture,
                                       AssetRegistry& registry,
                                       std::promise<size_t> registeredCountPromise,
                                       std::filesystem::path scanRoot,
                                       std::shared_ptr<const std::atomic<bool>> cancelRequested)
    : JobSystem::Task(jobSystem), m_MetadataFuture(std::move(metadataFuture)), m_Registry(registry),
      m_RegisteredCountPromise(std::move(registeredCountPromise)), m_ScanRoot(std::move(scanRoot)),
      m_CancelRequested(std::move(cancelRequested))
{
}

void RegistryUpdateTask::Execute()
{
    try
    {
        // Get processed metadata. Cancellation is checked only after it
        // resolves: this stage's future is the one AssetRegistry::Shutdown
        // joins, so it must not resolve while the metadata workers before it
        // still call into the registry.
        Vector<AssetMetadata> metadataList = m_MetadataFuture.get();

        if (IsCancelled())
        {
            m_RegisteredCountPromise.set_exception(ScanCancelledError());
            return;
        }

        // E2: the register fan-in used to be one serial loop doing per-file
        // GetOrCreateAssetGUID + RegisterAssetMetadata (per-file content hash,
        // CreateFileW, and two per-call-prepared SQLite statements — ~17µs of
        // prepare overhead each, one core). Shard it across JobSystem workers
        // (mirroring the stage-2 metadata fan-out) with each chunk registered
        // through RegisterAssetMetadataBatch: writer-lock scope and derived-
        // cache transaction per chunk, not per file.
        constexpr size_t kChunkSize = 256;
        const size_t chunkCount = (metadataList.size() + (kChunkSize - 1)) / kChunkSize;

        JobSystem::WorkStealingThreadPool* js = GetJobSystem();
        if (js && chunkCount > 1)
        {
            auto state = std::make_shared<RegisterFanoutState>();
            state->metadataList = std::move(metadataList);
            state->promise = std::move(m_RegisteredCountPromise);
            state->scanRoot = m_ScanRoot;
            state->cancelRequested = m_CancelRequested;

            // Throttle: leave half the pool for other async work, matching
            // the metadata stage's policy.
            const size_t workerCount = js->GetWorkerCount();
            const size_t maxWorkers = std::max<size_t>(1, workerCount > 1 ? (workerCount / 2) : 1);
            const size_t workersToSpawn = std::min(maxWorkers, chunkCount);
            state->activeWorkers.store(workersToSpawn, std::memory_order_relaxed);

            for (size_t i = 0; i < workersToSpawn; ++i)
            {
                auto task = std::make_unique<RegisterWorkerTask>(js, m_Registry, state, kChunkSize);
                js->Submit(std::move(task));
            }

            // The last worker fulfills the promise.
            return;
        }

        // Serial fallback (no job system / small scans): still batched.
        Vector<AssetMetadata> batch;
        batch.reserve(metadataList.size());
        for (AssetMetadata& md : metadataList)
        {
            if (IsCancelled())
            {
                m_RegisteredCountPromise.set_exception(ScanCancelledError());
                return;
            }
            if (md.Path.empty())
                continue;

            // Ensure the asset has an identity (GUID) without doing expensive
            // per-file work (hashing/fingerprinting) during startup scans.
            md.Guid = m_Registry.GetOrCreateAssetGUID(md.Path);
            if (md.Guid.IsNull())
                continue;
            batch.push_back(std::move(md));
        }
        std::vector<AssetDatabase::ReconcileScanNewFile> newFiles;
        const size_t registeredCount =
            batch.empty() ? 0 : m_Registry.RegisterAssetMetadataBatch(std::move(batch), &newFiles);

        // Same contract as the parallel fan-in's last worker: a cancelled
        // scan skips the reconcile.
        if (IsCancelled())
        {
            m_RegisteredCountPromise.set_exception(ScanCancelledError());
            return;
        }

        // Derived post-scan reconcile runs after the register batch and
        // before the promise resolves, so StartupScanFuture waiters observe
        // a fully reconciled registry.
        m_Registry.ReconcileDerivedSourceAfterScan(m_ScanRoot,
                                                   newFiles.empty() ? nullptr : &newFiles);

        Logger::Log::Debug("RegistryUpdateTask: Registered {} assets", registeredCount);

        m_RegisteredCountPromise.set_value(registeredCount);
    }
    catch (const AssetScanCancelled&)
    {
        m_RegisteredCountPromise.set_exception(std::current_exception());
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("RegistryUpdateTask failed: {}", e.what());
        m_RegisteredCountPromise.set_exception(std::current_exception());
    }
}

// AsyncRegistryCoordinator Implementation
AsyncRegistryCoordinator::AsyncRegistryCoordinator(AssetRegistry& registry, JobSystem::WorkStealingThreadPool& jobSystem)
    : m_Registry(registry), m_JobSystem(jobSystem)
{
}

std::future<size_t> AsyncRegistryCoordinator::ScanDirectoryAsync(const std::filesystem::path& directory, bool recursive)
{
    auto foundAssetsPromise = std::make_shared<std::promise<Vector<ScannedFileInfo>>>();
    auto processedMetadataPromise = std::make_shared<std::promise<Vector<AssetMetadata>>>();
    auto registeredCountPromise = std::make_shared<std::promise<size_t>>();

    auto foundAssetsFuture = foundAssetsPromise->get_future();
    auto processedMetadataFuture = processedMetadataPromise->get_future();
    auto registeredCountFuture = registeredCountPromise->get_future();

    // Stage 1: Directory scan
    SubmitDirectoryScanTask(directory, recursive, std::move(*foundAssetsPromise));

    // Stage 2: Metadata processing.
    // B.3b: copy the source's snapshot ONCE under the registry shared lock;
    // workers consult it lock-free to skip files whose (mtime, size) match.
    auto snapshotByPath = m_Registry.CopySnapshotByPathForRoot(directory);
    auto metadataTask = std::make_unique<MetadataBatchProcessingTask>(
        &m_JobSystem, std::move(foundAssetsFuture), m_Registry, std::move(*processedMetadataPromise),
        directory, std::move(snapshotByPath), m_CancelRequested);
    m_JobSystem.Submit(std::move(metadataTask));

    // Stage 3: Registry update
    SubmitRegistryUpdateTask(std::move(processedMetadataFuture), std::move(*registeredCountPromise), directory);

    return registeredCountFuture;
}

void AsyncRegistryCoordinator::CancelScans()
{
    m_CancelRequested->store(true);
}

void AsyncRegistryCoordinator::SubmitDirectoryScanTask(const std::filesystem::path& directory, bool recursive,
                                                       std::promise<Vector<ScannedFileInfo>> foundAssetsPromise)
{
    const AssetIgnoreRules rules = m_Registry.GetIgnoreRulesSnapshot();
    // C.2: pull the snapshot's per-directory mtime map once under the
    // registry's shared lock; the scan task uses it to skip subtree
    // recursion when a dir's mtime is unchanged since the snapshot.
    auto snapshotDirMtimes = m_Registry.CopyDirectoryMtimesByPathForRoot(directory);
    auto task = std::make_unique<DirectoryScanTask>(&m_JobSystem, directory, recursive, rules,
                                                    std::move(foundAssetsPromise), m_CancelRequested,
                                                    std::move(snapshotDirMtimes));
    m_JobSystem.Submit(std::move(task));
}

void AsyncRegistryCoordinator::SubmitRegistryUpdateTask(std::future<Vector<AssetMetadata>> metadataFuture,
                                                        std::promise<size_t> registeredCountPromise,
                                                        const std::filesystem::path& scanRoot)
{
    auto task = std::make_unique<RegistryUpdateTask>(&m_JobSystem, std::move(metadataFuture), m_Registry,
                                                     std::move(registeredCountPromise), scanRoot, m_CancelRequested);
    m_JobSystem.Submit(std::move(task));
}

} // namespace GameEngine

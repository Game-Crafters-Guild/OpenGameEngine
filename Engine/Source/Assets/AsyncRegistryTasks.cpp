#include "Assets/AsyncRegistryTasks.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/PathNormalization.h"
#include "AssetDatabase/AssetStoreReconciler.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
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

// B.3b: returns true if this scanned file is provably unchanged since the
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

// The units of the two scan fan-outs: 256 files per unit, claimed by the
// stage's own thread and its helpers.
constexpr size_t kScanChunkSize = 256;

size_t ScanChunkCount(size_t fileCount)
{
    return (fileCount + kScanChunkSize - 1) / kScanChunkSize;
}

// The fan-out width the two stages keep: the stage's own thread plus its
// helpers make max(1, W / 2) threads, leaving half the pool to other work.
// Without a job system the stage's own thread runs every unit.
JobSystem::ParallelForOptions ScanFanOut(const JobSystem::WorkStealingThreadPool* jobSystem,
                                         const std::function<bool()>& stop)
{
    JobSystem::ParallelForOptions options;
    options.Helpers = jobSystem ? std::max<size_t>(1, jobSystem->GetWorkerCount() / 2) - 1 : 0;
    options.Stop = &stop;
    return options;
}

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

    try
    {
        // Resolve the upstream file list (may block this worker thread, but will not block the caller thread).
        // Cancellation is checked only after it resolves, so this stage never finishes while the walk
        // before it still runs.
        const Vector<ScannedFileInfo> assetInfos = m_AssetInfosFuture.get();

        if (IsCancelled())
        {
            m_ProcessedMetadataPromise.set_exception(ScanCancelledError());
            return;
        }

        Logger::Log::Debug("MetadataBatchProcessingTask: Processing {} assets", assetInfos.size());

        // One unit per 256 files; each writes its own list, joined in file order
        // below. The cancel flag is read per file, so every thread stops mid-unit.
        const size_t chunkCount = ScanChunkCount(assetInfos.size());
        Vector<Vector<AssetMetadata>> chunkMetadata(chunkCount);

        JobSystem::WorkStealingThreadPool* js = GetJobSystem();
        const std::function<bool()> stop = [this] { return IsCancelled(); };
        const bool finished = JobSystem::ParallelFor(
            js, chunkCount, [&](size_t chunk) { return ProcessChunk(assetInfos, chunk, chunkMetadata[chunk]); },
            ScanFanOut(js, stop));

        // A cancelled scan left files unprocessed, so it is never handed on as complete.
        if (!finished || IsCancelled())
        {
            m_ProcessedMetadataPromise.set_exception(ScanCancelledError());
            return;
        }

        Vector<AssetMetadata> processedMetadata;
        size_t processedCount = 0;
        for (const Vector<AssetMetadata>& chunk : chunkMetadata)
            processedCount += chunk.size();
        processedMetadata.reserve(processedCount);
        for (Vector<AssetMetadata>& chunk : chunkMetadata)
            std::move(chunk.begin(), chunk.end(), std::back_inserter(processedMetadata));

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
    catch (...)
    {
        Logger::Log::Error("MetadataBatchProcessingTask failed with a non-standard exception");
        m_ProcessedMetadataPromise.set_exception(std::current_exception());
    }
}

// One unit of the metadata stage: the files of `chunk` into `out`. False once
// the scan is cancelled; the cancel flag is read per file.
bool MetadataBatchProcessingTask::ProcessChunk(const Vector<ScannedFileInfo>& assetInfos, size_t chunk,
                                               Vector<AssetMetadata>& out) const
{
    const size_t start = chunk * kScanChunkSize;
    const size_t end = std::min(start + kScanChunkSize, assetInfos.size());
    out.reserve(end - start);
    for (size_t i = start; i < end; ++i)
    {
        if (IsCancelled())
            return false;

        const ScannedFileInfo& info = assetInfos[i];
        // Cached entries came from the iterator and were live at scan
        // time; un-cached entries (BatchProcessAssetsAsync raw paths)
        // re-check existence.
        if (!info.hasCachedStats)
        {
            std::error_code ec;
            if (!std::filesystem::exists(info.path, ec))
                continue;
        }

        // B.3b: snapshot-current files are already in the registry
        // (populated by PopulateHotCachesFromSource at Initialize):
        // nothing to do downstream.
        if (IsScannedFileSnapshotCurrent(m_MountRoot, m_SnapshotByPath, info))
            continue;

        AssetMetadata metadata = ProcessSingleAssetForScan(m_Registry, info);
        if (!metadata.Path.empty())
            out.push_back(std::move(metadata));
    }
    return true;
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
        // joins, so it must not resolve while the stage before it still calls
        // into the registry.
        Vector<AssetMetadata> metadataList = m_MetadataFuture.get();

        if (IsCancelled())
        {
            m_RegisteredCountPromise.set_exception(ScanCancelledError());
            return;
        }

        // E2: one unit per 256 files resolves its GUIDs and registers them
        // through RegisterAssetMetadataBatch (one writer-lock scope and one
        // derived-cache transaction per unit, not per file). The cancel flag is
        // read on entry to each unit: a unit registers as a whole.
        const size_t chunkCount = ScanChunkCount(metadataList.size());
        std::atomic<size_t> registeredCount{0};
        std::vector<std::vector<AssetDatabase::ReconcileScanNewFile>> chunkNewFiles(chunkCount);

        JobSystem::WorkStealingThreadPool* js = GetJobSystem();
        const std::function<bool()> stop = [this] { return IsCancelled(); };
        const bool finished = JobSystem::ParallelFor(
            js, chunkCount,
            [&](size_t chunk) { return RegisterChunk(metadataList, chunk, chunkNewFiles[chunk], registeredCount); },
            ScanFanOut(js, stop));

        // A cancelled scan registered only some units. The reconcile takes its
        // rename-heal candidates from the registered files, so on a partial scan
        // it would miss heals and age ghosts a full scan heals; a cancelled scan
        // skips it.
        if (!finished || IsCancelled())
        {
            m_RegisteredCountPromise.set_exception(ScanCancelledError());
            return;
        }

        std::vector<AssetDatabase::ReconcileScanNewFile> newFiles;
        for (std::vector<AssetDatabase::ReconcileScanNewFile>& chunk : chunkNewFiles)
            std::move(chunk.begin(), chunk.end(), std::back_inserter(newFiles));

        // Derived post-scan reconcile runs after the last register batch and
        // before the promise resolves, so StartupScanFuture waiters observe a
        // fully reconciled registry.
        m_Registry.ReconcileDerivedSourceAfterScan(m_ScanRoot, newFiles.empty() ? nullptr : &newFiles);

        const size_t registered = registeredCount.load(std::memory_order_relaxed);
        Logger::Log::Debug("RegistryUpdateTask: Registered {} assets", registered);

        m_RegisteredCountPromise.set_value(registered);
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
    catch (...)
    {
        Logger::Log::Error("RegistryUpdateTask failed with a non-standard exception");
        m_RegisteredCountPromise.set_exception(std::current_exception());
    }
}

// One unit of the register stage: resolves the GUIDs of `chunk`'s files and
// registers them in one RegisterAssetMetadataBatch. False once the scan is
// cancelled; the flag is read on entry, so a unit registers as a whole.
bool RegistryUpdateTask::RegisterChunk(Vector<AssetMetadata>& metadataList, size_t chunk,
                                       std::vector<AssetDatabase::ReconcileScanNewFile>& newFiles,
                                       std::atomic<size_t>& registeredCount)
{
    if (IsCancelled())
        return false;
    const size_t start = chunk * kScanChunkSize;
    const size_t end = std::min(start + kScanChunkSize, metadataList.size());
    Vector<AssetMetadata> batch;
    batch.reserve(end - start);
    for (size_t i = start; i < end; ++i)
    {
        AssetMetadata& md = metadataList[i];
        if (md.Path.empty())
            continue;
        // Ensure the asset has an identity (GUID) without doing expensive
        // per-file work (hashing/fingerprinting) during startup scans.
        md.Guid = m_Registry.GetOrCreateAssetGUID(md.Path);
        if (md.Guid.IsNull())
            continue;
        batch.push_back(std::move(md));
    }
    if (!batch.empty())
        registeredCount.fetch_add(m_Registry.RegisterAssetMetadataBatch(std::move(batch), &newFiles),
                                  std::memory_order_relaxed);
    return true;
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

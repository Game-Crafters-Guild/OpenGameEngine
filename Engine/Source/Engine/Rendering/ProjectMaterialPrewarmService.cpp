#include "Engine/Rendering/ProjectMaterialPrewarmService.h"

#include "Core/CpuProfiler.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Logger/Logger.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>

namespace GameEngine::Engine::Renderer
{
namespace
{
struct PendingMaterial
{
    GUID Guid;
    std::filesystem::path Path;
    std::optional<MaterialDocument> EmbeddedDocument;
};

// A .material the warm-up takes: inside the open project's asset root, in a
// source whose content can change (a packaged game's is fixed).
bool IsWarmableMaterial(const AssetRegistry& registry, const std::filesystem::path& assetRoot,
                        const std::filesystem::path& path)
{
    std::string relative;
    return AssetPaths::TryMakeCanonicalRelativePath(assetRoot, path, relative) &&
           registry.AcceptsDerivedRecords(path);
}

// A model's embedded materials, from the derived-cache record its last load
// wrote, converted the way model registration converts them.
void AppendRecordedModelMaterials(const AssetRegistry& registry, const AssetIndexRecord& row,
                                  Vector<PendingMaterial>& result)
{
    const auto file = ImportedMaterialCacheFile(registry, row.Path, row.Guid);
    if (!file)
        return;
    const auto materials = ReadImportedMaterialCache(*file);
    if (!materials)
        return;
    for (uint32 i = 0; i < static_cast<uint32>(materials->size()); ++i)
    {
        ConvertedModelMaterial converted = ModelMaterialBridge::Convert(row.Guid, i, (*materials)[i]);
        result.push_back({{}, {}, std::move(converted.document)});
    }
}

// Every project material and recorded model material under `assetRoot`. Runs on
// a pool worker: registry reads and small record files only.
Vector<PendingMaterial> ListProjectMaterials(const AssetRegistry& registry,
                                             const std::filesystem::path& assetRoot)
{
    Vector<PendingMaterial> materials;
    for (const AssetIndexRecord& row : registry.GetAssetIndexSnapshot())
    {
        if (row.Type == AssetType::Material)
        {
            if (IsWarmableMaterial(registry, assetRoot, row.Path))
                materials.push_back({row.Guid, row.Path, std::nullopt});
        }
        else if (row.Type == AssetType::Model)
        {
            std::string relative;
            if (AssetPaths::TryMakeCanonicalRelativePath(assetRoot, row.Path, relative))
                AppendRecordedModelMaterials(registry, row, materials);
        }
    }
    return materials;
}

// .material files created or edited while the project is open. Asset events
// arrive on whichever thread raised them (file changes on the watcher thread),
// so the handler only records the GUID and the pump reads the registry on the
// main thread. Shared with the dispatcher's copy of the handler, which can still
// run once after the handler is removed.
struct MaterialEventInbox
{
    std::mutex Mutex;
    Vector<GUID> Materials;
    std::atomic<bool> HasMaterials{false};

    Vector<GUID> Take()
    {
        Vector<GUID> taken;
        if (!HasMaterials.exchange(false, std::memory_order_acquire))
            return taken;
        std::lock_guard<std::mutex> lock(Mutex);
        taken.swap(Materials);
        return taken;
    }
};
} // namespace

struct ProjectMaterialPrewarmService::State
{
    using Clock = std::chrono::steady_clock;
    std::filesystem::path Root;
    uint64_t Epoch = 0;
    uint64_t SnapshotEpoch = 0;
    JobSystem::TaskHandle Snapshot;
    // The open project has not been listed yet.
    bool ListingPending = false;
    Clock::time_point NextWork{};
    Clock::time_point WorkStarted{};
    Vector<PendingMaterial> Queue;
    std::optional<AssetFuture> Pending;
    bool AwaitingShaders = false;

    std::shared_ptr<MaterialEventInbox> Inbox = std::make_shared<MaterialEventInbox>();
    AssetEventDispatcher* Dispatcher = nullptr;
    uint32 CallbackHandle = 0;

    // The handler also retires a deleted model's record, the warm-up being the
    // record's reader. Dispatch happens inside the AssetManager that owns both
    // the dispatcher and `registry`, so the registry outlives every call.
    void Subscribe(AssetManager& assets)
    {
        AssetEventDispatcher& dispatcher = assets.GetEventDispatcher();
        if (Dispatcher == &dispatcher)
            return;
        Unsubscribe();
        Dispatcher = &dispatcher;
        CallbackHandle = dispatcher.AddCallback(
            [inbox = Inbox, registry = &assets.GetRegistry()](const AssetEvent& event)
        {
            if (event.Type == AssetType::Model && event.EventType == AssetEventType::AssetDestroyed &&
                !event.AssetPath.empty())
            {
                RemoveImportedMaterialCache(*registry, event.AssetPath, event.AssetGuid);
                return;
            }
            if (event.Type != AssetType::Material ||
                (event.EventType != AssetEventType::AssetCreated &&
                 event.EventType != AssetEventType::AssetModified))
                return;
            std::lock_guard<std::mutex> lock(inbox->Mutex);
            inbox->Materials.push_back(event.AssetGuid);
            inbox->HasMaterials.store(true, std::memory_order_release);
        });
    }

    void Unsubscribe()
    {
        if (Dispatcher)
            Dispatcher->RemoveCallback(CallbackHandle);
        Dispatcher = nullptr;
    }

    void QueueMaterialEvents(const AssetRegistry& registry, const std::filesystem::path& assetRoot)
    {
        for (const GUID& guid : Inbox->Take())
        {
            AssetMetadata metadata;
            if (registry.TryGetAssetMetadata(guid, metadata) && metadata.Type == AssetType::Material &&
                IsWarmableMaterial(registry, assetRoot, metadata.Path))
                Queue.push_back({guid, metadata.Path, std::nullopt});
        }
    }

    void FinishWork(Clock::time_point now)
    {
        // One non-preemptible material batch, then a proportional quiet period.
        // Admission targets 5% of one worker's elapsed time, including asset I/O
        // and time spent waiting for foreground compiles. Never sleep a worker.
        NextWork = now + std::max(Clock::duration(std::chrono::milliseconds(100)),
                                  (now - WorkStarted) * 19);
    }
};

ProjectMaterialPrewarmService::ProjectMaterialPrewarmService() : m_State(std::make_unique<State>()) {}
ProjectMaterialPrewarmService::~ProjectMaterialPrewarmService()
{
    m_State->Unsubscribe();
    // Discovery reads the registry. Join before the engine can destroy its
    // AssetManager; a cancelled pool task is terminal too.
    if (m_State->Snapshot.IsValid())
        m_State->Snapshot.Wait();
}

void ProjectMaterialPrewarmService::Update(
    AssetManager& assets, MaterialSystem& materials, JobSystem::WorkStealingThreadPool& pool,
    const std::filesystem::path& projectRoot, std::chrono::steady_clock::time_point now,
    const std::function<Rendering::MaterialKeyword()>& resolveWorldPassKeywords)
{
    GE_CPU_PROFILE_SCOPE("MaterialSystem.ProjectMaterialPrewarm");
    using namespace std::chrono_literals;
    auto& state = *m_State;
    state.Subscribe(assets);
    if (state.Root.native() != projectRoot.native())
    {
        state.Root = projectRoot;
        ++state.Epoch;
        state.Queue.clear();
        state.Pending.reset();
        state.AwaitingShaders = false;
        state.ListingPending = !projectRoot.empty();
        state.NextWork = now + 2s;
        // The listing covers every material an earlier event named.
        state.Inbox->Take();
    }

    if (state.Snapshot.IsValid())
    {
        if (!state.Snapshot.IsDone())
            return;
        std::shared_ptr<Vector<PendingMaterial>> rows;
        if (state.Snapshot.TryGetResult(rows) && rows &&
            !projectRoot.empty() && state.SnapshotEpoch == state.Epoch)
        {
            if (!rows->empty())
                Logger::Log::Info("Project material warm-up: queued {} material document(s)", rows->size());
            state.Queue.insert(state.Queue.end(), std::make_move_iterator(rows->begin()),
                               std::make_move_iterator(rows->end()));
            state.FinishWork(now);
        }
        else if (state.Snapshot.HasFailed())
        {
            Logger::Log::Warning("Project material warm-up: registry discovery failed: {}",
                                 state.Snapshot.GetErrorMessage());
        }
        state.Snapshot = {};
        return;
    }
    if (projectRoot.empty())
    {
        state.Inbox->Take();
        return;
    }
    state.QueueMaterialEvents(assets.GetRegistry(), assets.GetAssetRoot());

    // Demand prewarms take precedence. A project batch cannot overlap another
    // batch, and a completed batch repays its elapsed-time budget before another
    // asset read or compilation can begin.
    const auto demand = materials.GetPrewarmProgress();
    if (demand.submitted != demand.completed)
        return;
    if (state.AwaitingShaders)
    {
        const auto warmUp = materials.GetProjectWarmUpProgress();
        if (warmUp.submitted != warmUp.completed)
            return;
        state.AwaitingShaders = false;
        state.FinishWork(now);
        return;
    }

    if (state.Pending)
    {
        if (state.Pending->wait_for(0ms) != std::future_status::ready)
            return;
        SharedPtr<Asset> loaded;
        try
        {
            loaded = state.Pending->get();
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Project material warm-up: material load failed: {}", e.what());
        }
        state.Pending.reset();
        if (auto* material = dynamic_cast<MaterialAsset*>(loaded.get()))
        {
            materials.PrewarmMaterialShaders(material->GetDocument(), material->GetPath(),
                                             resolveWorldPassKeywords());
            state.AwaitingShaders = true;
        }
        else
            state.FinishWork(now);
        return;
    }

    if (now < state.NextWork || pool.GetApproximateQueueSize() != 0 ||
        pool.GetPendingTasksApprox() >= pool.GetWorkerCount())
        return;

    // The listing waits for the project's directory scan, which registers files
    // added while the project was closed without raising asset events. It runs
    // on the shared Background lane, never a private std::async thread.
    if (state.ListingPending)
    {
        if (assets.GetRegistry().IsStartupScanRunning(kAssetSourceAliasProject))
            return;
        state.ListingPending = false;
        state.SnapshotEpoch = state.Epoch;
        state.WorkStarted = now;
        state.Snapshot = pool.Submit([&assets, assetRoot = assets.GetAssetRoot()]
        {
            return std::make_shared<Vector<PendingMaterial>>(
                ListProjectMaterials(assets.GetRegistry(), assetRoot));
        }, JobSystem::JobPriority::Background);
        return;
    }
    if (state.Queue.empty())
        return;

    state.WorkStarted = now;
    PendingMaterial item = std::move(state.Queue.back());
    state.Queue.pop_back();
    if (item.EmbeddedDocument)
    {
        materials.PrewarmMaterialShaders(*item.EmbeddedDocument, {}, resolveWorldPassKeywords());
        state.AwaitingShaders = true;
    }
    else if (!assets.IsLoadSuppressed(item.Guid))
        state.Pending.emplace(assets.LoadAssetAsync(item.Guid, AssetLoadPriority::Low));
    else
        state.FinishWork(now);
}

} // namespace GameEngine::Engine::Renderer

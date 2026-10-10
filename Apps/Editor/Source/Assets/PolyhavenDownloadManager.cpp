#include "Assets/PolyhavenDownloadManager.h"

#include "Assets/AssetCreation.h"
#include "Assets/PolyhavenService.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "JobSystem/JobChannel.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Entities/SpriteEntityFactory.h"
#include "Editor/Assets/AsyncAssetHelpers.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "UndoRedo/TextureDropOnMeshCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{
std::string HDRIKey(const std::string& slug, const std::string& resolution)
{
    return slug + "|" + (resolution.empty() ? std::string("1k") : resolution);
}

int HDRIResolutionRank(std::string_view resolution)
{
    if (resolution == "16k") return 5;
    if (resolution == "8k") return 4;
    if (resolution == "4k") return 3;
    if (resolution == "2k") return 2;
    if (resolution == "1k" || resolution.empty()) return 1;
    return 0;
}

// The world every placeholder and assignment target belongs to. Record and
// spend sites must read the reset generation from the same world, or the
// comparison that detects a scene swap is meaningless.
ECS::World* TargetWorld()
{
    return EngineCore::GetInstance().EnsurePrimaryWorld();
}
}

// At most four transfers at once (design decision D4): each holds a blocking thread for
// the length of its transfer, and more at once only split the same bandwidth.
constexpr uint32 kMaxConcurrentTransfers = 4;

PolyhavenDownloadManager::PolyhavenDownloadManager() = default;

PolyhavenDownloadManager::~PolyhavenDownloadManager()
{
    Shutdown();
}

JobSystem::JobChannel* PolyhavenDownloadManager::Transfers()
{
    if (!m_Transfers && !m_ShutDown)
        m_Transfers = std::make_unique<JobSystem::JobChannel>(
            EngineCore::GetInstance().GetJobSystem(),
            JobSystem::JobChannelDesc{.Name = "Polyhaven", .MaxRunning = kMaxConcurrentTransfers});
    return m_Transfers.get();
}

void PolyhavenDownloadManager::FinishDownload(const std::shared_ptr<SharedState>& state,
                                              uint32_t id,
                                              CompletedDownload&& result)
{
    {
        std::lock_guard<std::mutex> lock(state->Mutex);
        if (!state->ShuttingDown)
        {
            state->Completed.push_back(std::move(result));
            state->Active.erase(
                std::remove_if(state->Active.begin(), state->Active.end(),
                               [id](const DownloadRequest& r) { return r.downloadId == id; }),
                state->Active.end());
        }
    }
}

void PolyhavenDownloadManager::LaunchDownloadTask(std::function<void()> task)
{
    // Submitted after SharedState::Mutex is dropped — the JobSystem's inline mode
    // would otherwise re-enter that lock on this thread. A transfer still queued at
    // quit is cancelled (the channel's Submit contract); a running one finishes into
    // SharedState, which outlives `this` for exactly that case.
    JobSystem::JobChannel* transfers = Transfers();
    if (!transfers)
    {
        Logger::Log::Warning("PolyhavenDownloadManager: a download started after Shutdown was not run");
        return;
    }
    (void)transfers->Submit(std::move(task));
}

uint32_t PolyhavenDownloadManager::StartDownload(const std::string& slug,
                                                  const std::string& assetType,
                                                  const std::filesystem::path& destDir,
                                                  const ScopedEntity& placeholder,
                                                  const float savedTransform[10])
{
    uint32_t id = 0;
    std::function<void()> downloadTask;
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);

        m_Shared->CompletedSlugs.erase(slug);

        if (m_Shared->ShuttingDown)
            return 0;

        DownloadRequest req{};
        req.downloadId = m_Shared->NextId++;
        req.slug = slug;
        req.assetType = assetType;
        if (req.assetType == "hdris")
            req.resolution = PolyhavenService::GetPreferredHDRIResolution();
        req.destDir = destDir;
        req.Placeholder = placeholder;
        std::memcpy(req.savedTransform, savedTransform, sizeof(req.savedTransform));

        m_Shared->Active.push_back(req);

        id = req.downloadId;
        const std::string slugCopy = req.slug;
        const std::string typeCopy = req.assetType;
        const std::string resolutionCopy = req.resolution;
        const std::filesystem::path dirCopy = req.destDir;
        const ScopedEntity entityCopy = req.Placeholder;
        std::array<float, 10> transformCopy{};
        std::memcpy(transformCopy.data(), req.savedTransform, sizeof(req.savedTransform));

        downloadTask =
            [state = m_Shared, id, slugCopy, typeCopy, resolutionCopy, dirCopy, entityCopy,
             transformCopy]()
            {
                auto onProgress = [state, slugCopy](size_t done, size_t total) {
                    std::lock_guard<std::mutex> lk(state->Mutex);
                    state->Progress[slugCopy] = {static_cast<uint32_t>(done),
                                                 static_cast<uint32_t>(total)};
                };
                std::filesystem::path mainFile = (typeCopy == "hdris" && !resolutionCopy.empty())
                    ? PolyhavenService::DownloadHDRI(slugCopy, dirCopy, resolutionCopy)
                    : PolyhavenService::DownloadAsset(slugCopy, typeCopy, dirCopy, onProgress);

                CompletedDownload result{};
                result.downloadId = id;
                result.slug = slugCopy;
                result.assetType = typeCopy;
                result.resolution = resolutionCopy;
                result.mainFilePath = std::move(mainFile);
                result.Placeholder = entityCopy;
                std::memcpy(result.savedTransform, transformCopy.data(), sizeof(result.savedTransform));
                result.success = !result.mainFilePath.empty();
                FinishDownload(state, id, std::move(result));
            };
    }

    LaunchDownloadTask(std::move(downloadTask));
    Logger::Log::Info("PolyhavenDownloadManager: started download #{} for '{}'", id, slug);
    return id;
}

uint32_t PolyhavenDownloadManager::StartDownloadForPlaceholder(const std::string& slug,
                                                                const std::string& assetType,
                                                                const std::filesystem::path& assetsRoot,
                                                                ECS::EntityHandle entity,
                                                                ECS::World* world)
{
    std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
    float savedTransform[10] = {};
    if (world)
    {
        if (auto* xf = world->GetComponent<Components::Transform>(entity))
        {
            savedTransform[0] = xf->matrix[12]; // posX
            savedTransform[1] = xf->matrix[13]; // posY
            savedTransform[2] = xf->matrix[14]; // posZ
            savedTransform[3] = 0.0f; // rotX
            savedTransform[4] = 0.0f; // rotY
            savedTransform[5] = 0.0f; // rotZ
            savedTransform[6] = 1.0f; // rotW
            savedTransform[7] = 1.0f; // scaleX
            savedTransform[8] = 1.0f; // scaleY
            savedTransform[9] = 1.0f; // scaleZ
        }
    }
    return StartDownload(slug, assetType, destDir,
                         ScopedEntity::Capture(entity, TargetWorld()), savedTransform);
}

void PolyhavenDownloadManager::PollCompleted(std::vector<CompletedDownload>& out)
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    if (m_Shared->Completed.empty())
        return;
    out.insert(out.end(),
               std::make_move_iterator(m_Shared->Completed.begin()),
               std::make_move_iterator(m_Shared->Completed.end()));
    m_Shared->Completed.clear();
}

void PolyhavenDownloadManager::SetContext(const EditorContext* ctx,
                                           Editor::EditorChangeNotifications* notifications)
{
    m_Context = ctx;
    m_Notifications = notifications;
}

void PolyhavenDownloadManager::PollAndProcess()
{
    std::vector<CompletedDownload> completed;
    PollCompleted(completed);

    // Apply placeholder assignments from the durable map. The thread lambda
    // captures entity handles at launch time, so early downloads will have
    // invalid handles. The map is updated by AssignPlaceholder() on the
    // main thread and is the authoritative source.
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);
        for (auto& dl : completed)
        {
            auto it = m_Shared->PlaceholderAssignments.find(dl.slug);
            if (it != m_Shared->PlaceholderAssignments.end())
            {
                dl.Placeholder = it->second.Placeholder;
                std::memcpy(dl.savedTransform, it->second.savedTransform, sizeof(dl.savedTransform));
                m_Shared->PlaceholderAssignments.erase(it);
            }
        }
    }

    // Process at most one replacement per frame to avoid stalling the
    // render loop with multiple synchronous model loads.
    bool replacedOne = false;
    ECS::World* const world = TargetWorld();
    for (auto& dl : completed)
    {
        // Check if this download has a multi-texture assignment pending (PBR maps).
        auto multiTexIt = m_Shared->MultiTextureAssignments.find(dl.slug);
        if (multiTexIt != m_Shared->MultiTextureAssignments.end())
        {
            // Multi-map texture drop on mesh — assign all PBR maps.
            const MultiTextureAssignment& multiAssign = multiTexIt->second;
            if (multiAssign.Target.Resolve(world).IsValid() && dl.success && !dl.mainFilePath.empty())
            {
                ProcessMultiTextureAssignment(dl, multiAssign);
            }
            m_Shared->MultiTextureAssignments.erase(multiTexIt);
            continue;
        }

        // Check if this download has an HDRI skybox assignment pending.
        const std::string hdriKey = (dl.assetType == "hdris") ? HDRIKey(dl.slug, dl.resolution) : dl.slug;
        auto hdriIt = m_Shared->HDRISkyboxAssignments.find(hdriKey);
        if (hdriIt != m_Shared->HDRISkyboxAssignments.end())
        {
            const HDRISkyboxAssignment hdriAssign = hdriIt->second;
            const ECS::EntityHandle skybox = hdriAssign.Target.Resolve(world);
            if (skybox.IsValid() && dl.success && !dl.mainFilePath.empty())
            {
                ProcessHDRISkyboxAssignment(dl, hdriAssign);
            }
            else if (hdriAssign.notifications && skybox.IsValid())
            {
                Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
                rebuild.world = world;
                rebuild.entity = skybox;
                rebuild.componentType = ECS::GetComponentTypeId<Components::Skybox>();
                rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                hdriAssign.notifications->NotifyComponentChanged(rebuild);
            }
            m_Shared->HDRISkyboxAssignments.erase(hdriIt);
            if (m_OnDownloadFinished)
                m_OnDownloadFinished();
            continue;
        }

        // A world reset while this download was in flight already destroyed the
        // placeholder and restarted entity versions, so the cached handle now
        // aliases an unrelated entity of the incoming scene. Forget the
        // placeholder without destroying anything, and keep the finished
        // download: the file is on disk, and parking it here lets a fresh drop
        // of the same asset resolve without downloading it again.
        if (dl.Placeholder.WasRecorded() && dl.Placeholder.IsStale(world))
        {
            Logger::Log::Info(
                "Polyhaven: world was reset while '{}' downloaded — placeholder dropped, asset kept",
                dl.slug);
            dl.Placeholder = ScopedEntity{};
        }

        if (!dl.Placeholder.WasRecorded())
        {
            // No placeholder assigned yet — store so AssignPlaceholder() can
            // schedule immediate replacement when the user drops the asset.
            std::lock_guard<std::mutex> lock(m_Shared->Mutex);
            const std::string completedKey = (dl.assetType == "hdris")
                ? HDRIKey(dl.slug, dl.resolution)
                : dl.slug;
            m_Shared->CompletedSlugs[completedKey] = std::move(dl);
            continue;
        }
        if (replacedOne)
        {
            // Re-queue for next frame.
            std::lock_guard<std::mutex> lock(m_Shared->Mutex);
            m_Shared->Completed.push_back(std::move(dl));
            continue;
        }
        ReplacePlaceholder(dl);
        replacedOne = true;
        if (m_OnDownloadFinished)
            m_OnDownloadFinished();
    }

    m_PillOverlay.Update(world);
}

void PolyhavenDownloadManager::ReplacePlaceholder(const CompletedDownload& dl)
{
    // Taken at entry, so a retry that returns early (a moved world, a failed move into the
    // project) cannot leave the slug marked and make a later download of it skip the wait.
    const bool loadFinished = m_LoadsFinished.erase(dl.slug) > 0;
    ECS::World* world = TargetWorld();

    // Resolve, never trust: a handle cached before a world reset still passes
    // World::IsValid while naming an unrelated entity of the incoming scene, and
    // destroying it here would delete a freshly loaded entity the user never
    // touched. Resolve() yields an invalid handle in that case, which makes every
    // destroy and every component read below a no-op.
    const ECS::EntityHandle placeholder = dl.Placeholder.Resolve(world);
    const bool placeholderValid = placeholder.IsValid();

    // Helper: destroy placeholder and notify on failure.
    auto destroyPlaceholder = [&]() {
        if (placeholderValid)
        {
            world->DestroyEntityImmediate(placeholder);
            if (m_Notifications)
            {
                Editor::EditorChangeNotifications::WorldStructureChangedEvent evt{};
                evt.world = world;
                evt.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
                m_Notifications->NotifyWorldStructureChanged(evt);
            }
        }
    };

    if (!dl.success || dl.mainFilePath.empty())
    {
        Logger::Log::Warning("Polyhaven download failed for '{}'", dl.slug);
        destroyPlaceholder();
        return;
    }

    if (!dl.Placeholder.WasRecorded())
    {
        Logger::Log::Info("Polyhaven: early download completed for '{}' (no placeholder)", dl.slug);
        return;
    }

    if (!world)
        return;

    // The drop targeted a scene that has since been swapped out, so there is no
    // placeholder left to replace and nothing in the incoming scene asked for
    // this model. Spawning it here would dump geometry into a scene the user
    // never dropped it into; the asset itself is already on disk and in the
    // asset browser, so nothing is lost by leaving placement to a fresh drop.
    // PollAndProcess clears a stale placeholder before it gets here; this is the
    // check at the function that actually destroys and spawns, which should not
    // depend on its caller having done it.
    if (dl.Placeholder.IsStale(world))
    {
        Logger::Log::Info("Polyhaven: world was reset while '{}' downloaded — not placing it", dl.slug);
        return;
    }

    // Read position, parent, and sibling order from placeholder (if it still exists).
    float posX = dl.savedTransform[0], posY = dl.savedTransform[1], posZ = dl.savedTransform[2];
    ECS::EntityHandle parentEntity{};
    bool hasOrder = false;
    Components::HierarchyOrder savedOrder{};
    if (placeholderValid)
    {
        if (auto* xf = world->GetComponent<Components::Transform>(placeholder))
        {
            posX = xf->matrix[12];
            posY = xf->matrix[13];
            posZ = xf->matrix[14];
        }
        if (auto* p = world->GetComponent<Components::Parent>(placeholder))
            parentEntity = p->parent;
        if (auto* o = world->GetComponent<Components::HierarchyOrder>(placeholder))
        {
            hasOrder = true;
            savedOrder = *o;
        }
    }

    if (!m_Context || !m_Context->Assets)
    {
        Logger::Log::Warning("Polyhaven: no asset manager for '{}'", dl.slug);
        destroyPlaceholder();
        return;
    }

    // Re-resolve the file path — SetDropPreview may have already moved files
    // from the temp cache to the project directory.
    std::filesystem::path mainFilePath = PolyhavenService::FindDownloadedFile(dl.slug, m_Context->AssetsRoot);
    if (mainFilePath.empty())
        mainFilePath = PolyhavenService::FindCachedDownloadFile(dl.slug);
    if (mainFilePath.empty())
        mainFilePath = dl.mainFilePath; // fallback to stored path

    // If still in temp cache, move to project assets.
    std::filesystem::path cacheDownloadsDir = PolyhavenService::GetCacheDir() / "downloads";
    std::error_code ec;
    std::filesystem::path canonicalMain = std::filesystem::canonical(mainFilePath, ec);
    std::filesystem::path canonicalCache = std::filesystem::canonical(cacheDownloadsDir, ec);
    if (!ec && !canonicalMain.empty() && !canonicalCache.empty() &&
        canonicalMain.string().starts_with(canonicalCache.string()))
    {
        std::filesystem::path projectDir = m_Context->AssetsRoot / "Polyhaven" / dl.slug;
        std::filesystem::path srcDir = mainFilePath.parent_path();
        mainFilePath = PolyhavenService::MoveDownloadToProject(mainFilePath, srcDir, projectDir);
        if (mainFilePath.empty())
        {
            Logger::Log::Warning("Polyhaven: failed to move download to project for '{}'", dl.slug);
            destroyPlaceholder();
            return;
        }
    }

    auto& am = *m_Context->Assets;
    GUID assetGuid = am.ResolveAssetGuid(mainFilePath);
    if (assetGuid.IsNull())
    {
        Logger::Log::Warning("Polyhaven: failed to resolve asset GUID for '{}'", mainFilePath.string());
        destroyPlaceholder();
        return;
    }

    // Check if this is a texture or model download
    const std::string ext = mainFilePath.extension().string();
    const bool isTexture = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr" || ext == ".exr");
    const bool isModel = (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".fbx");

    SharedPtr<Asset> asset = am.GetAsset(assetGuid);
    if ((isTexture || isModel) && !asset && !loadFinished)
    {
        // Not loaded yet: the placeholder and its download pill stay, and the replacement
        // is queued again once the load has finished; it is never waited for. A load that
        // finished without an asset is the failure below.
        Editor::RunWhenAssetLoaded(am, assetGuid, AssetLoadPriority::High, nullptr, nullptr,
            [this, dl]()
            {
                m_LoadsFinished.insert(dl.slug);
                std::lock_guard<std::mutex> lock(m_Shared->Mutex);
                m_Shared->Completed.push_back(dl);
            },
            mainFilePath.filename().string());
        return;
    }

    if (isTexture)
    {
        if (!asset)
        {
            // The load finished without a texture: say so instead of leaving the placeholder.
            Logger::Log::Warning("Polyhaven: '{}' downloaded but its texture did not load (the asset load error "
                                 "above names why); the placeholder was removed. Drop the texture from the Assets "
                                 "panel to retry.",
                                 dl.slug);
            destroyPlaceholder();
            return;
        }
        // Handle texture download completion - create sprite entity
        auto* rs = m_Context->RenderServices;
        if (!rs)
        {
            Logger::Log::Warning("Polyhaven: RenderServices not available for texture '{}'", dl.slug);
            destroyPlaceholder();
            return;
        }

        // Create sprite entity from texture
        Mathematics::Vector3 pos{posX, posY, posZ};
        ECS::EntityHandle spriteResult = Editor::CreateSpriteEntityFromTexture(
            *world, *rs, am, mainFilePath, pos);
        if (!spriteResult.IsValid())
        {
            Logger::Log::Warning("Polyhaven: failed to create sprite entity for texture '{}'", dl.slug);
            destroyPlaceholder();
            return;
        }

        // Destroy placeholder and use sprite entity
        if (placeholderValid)
            world->DestroyEntityImmediate(placeholder);

        // Apply parent if there was one
        if (parentEntity.IsValid() && world->IsValid(parentEntity))
        {
            Components::Parent p{};
            p.parent = parentEntity;
            world->AddComponentImmediate(spriteResult, p);
        }
        if (hasOrder)
            world->AddComponentImmediate(spriteResult, savedOrder);

        // Notify UI to refresh hierarchy
        if (m_Notifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent evt{};
            evt.world = world;
            evt.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            m_Notifications->NotifyWorldStructureChanged(evt);
        }

        if (m_OnEntityCreated)
            m_OnEntityCreated(spriteResult);

        Logger::Log::Info("Polyhaven: replaced placeholder with sprite for texture '{}'", dl.slug);
    }
    else if (isModel)
    {
        auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
        auto* rs = m_Context->RenderServices;
        if (!modelAsset || !modelAsset->IsLoaded() || !rs)
        {
            Logger::Log::Warning("Polyhaven: failed to load model for '{}' (asset={}, model={}, loaded={}, rs={})",
                dl.slug, asset ? "yes" : "null",
                modelAsset ? "yes" : "cast_fail",
                modelAsset ? (modelAsset->IsLoaded() ? "yes" : "no") : "n/a",
                rs ? "yes" : "null");
            destroyPlaceholder();
            return;
        }

        // Don't display the model until all textures are loaded. An image without
        // content is an external texture file that wasn't found — re-queue and retry
        // next frame (the file may appear once the move completes).
        const auto& images = modelAsset->GetEmbeddedImages();
        bool allTexturesReady = true;
        for (const auto& img : images)
        {
            if (!img.HasContent())
            {
                allTexturesReady = false;
                break;
            }
        }
        if (!allTexturesReady)
        {
            Logger::Log::Info("Polyhaven: textures not ready for '{}', retrying next frame", dl.slug);
            am.UnloadAssetAsync(assetGuid).wait();
            std::lock_guard<std::mutex> lock(m_Shared->Mutex);
            m_Shared->Completed.push_back(dl);
            return;
        }

        auto result = Engine::Renderer::ModelEntityFactory::CreateFromModel(
            *rs, *world, *modelAsset, assetGuid, dl.slug, Editor::GetFbxModelEntityFactoryOptions(mainFilePath));
        if (!result.IsValid())
        {
            Logger::Log::Warning("Polyhaven: failed to create model entity for '{}'", dl.slug);
            destroyPlaceholder();
            return;
        }

        // Export derived materials as .material files so the inspector can edit them.
        Editor::ExportModelMaterials(mainFilePath, assetGuid, result.submeshEntities, *world, am);

        // Model created successfully — now destroy the placeholder.
        if (placeholderValid)
            world->DestroyEntityImmediate(placeholder);

        if (auto* xf = world->GetComponentForWrite<Components::Transform>(result.rootEntity))
        {
            xf->SetIdentity();
            xf->matrix[12] = posX;
            xf->matrix[13] = posY;
            xf->matrix[14] = posZ;
        }
        if (parentEntity.IsValid() && world->IsValid(parentEntity))
        {
            Components::Parent p{};
            p.parent = parentEntity;
            world->AddComponentImmediate(result.rootEntity, p);
        }
        if (hasOrder)
            world->AddComponentImmediate(result.rootEntity, savedOrder);

        // Notify UI to refresh hierarchy and other panels.
        if (m_Notifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent evt{};
            evt.world = world;
            evt.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            m_Notifications->NotifyWorldStructureChanged(evt);
        }

        if (m_OnEntityCreated)
            m_OnEntityCreated(result.rootEntity);

        Logger::Log::Info("Polyhaven: replaced placeholder with full model for '{}'", dl.slug);
    }
    else
    {
        Logger::Log::Warning("Polyhaven: unknown asset type for '{}' ext={}", dl.slug, ext);
        destroyPlaceholder();
    }
}

bool PolyhavenDownloadManager::HasActiveDownloads() const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    return !m_Shared->Active.empty();
}

uint32_t PolyhavenDownloadManager::ActiveDownloadCount() const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    return static_cast<uint32_t>(m_Shared->Active.size());
}

void PolyhavenDownloadManager::GetActiveDownloadSlugs(std::vector<std::string>& out) const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    out.reserve(out.size() + m_Shared->Active.size());
    for (const auto& r : m_Shared->Active)
        out.push_back(r.slug);
}

void PolyhavenDownloadManager::GetActivePlaceholderDownloads(
    const ECS::World* world,
    std::vector<std::pair<std::string, ECS::EntityHandle>>& out) const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    out.reserve(out.size() + m_Shared->PlaceholderAssignments.size() + m_Shared->Active.size());

    // The assignment map holds placeholders attached via AssignPlaceholder (drop on
    // an already-running download); it overrides the request placeholder, so it wins.
    for (const auto& kv : m_Shared->PlaceholderAssignments)
    {
        const ECS::EntityHandle e = kv.second.Placeholder.Resolve(world);
        if (e.IsValid())
            out.emplace_back(kv.first, e);
    }

    // A fresh drop starts via StartDownloadForPlaceholder, which carries the
    // placeholder on the active request rather than in the map. Add those too.
    for (const auto& r : m_Shared->Active)
    {
        if (m_Shared->PlaceholderAssignments.count(r.slug))
            continue;
        const ECS::EntityHandle e = r.Placeholder.Resolve(world);
        if (e.IsValid())
            out.emplace_back(r.slug, e);
    }
}

bool PolyhavenDownloadManager::GetDownloadProgress(const std::string& slug, float& outFraction) const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    auto it = m_Shared->Progress.find(slug);
    if (it == m_Shared->Progress.end() || it->second.second == 0)
        return false;
    const float f = static_cast<float>(it->second.first) / static_cast<float>(it->second.second);
    outFraction = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return true;
}

bool PolyhavenDownloadManager::TryGetPendingHdriSkyboxResolution(ECS::EntityHandle entity,
                                                               std::string& outResolution) const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    ECS::World* const world = TargetWorld();
    for (const auto& kv : m_Shared->HDRISkyboxAssignments)
    {
        if (kv.second.Target.Resolve(world) != entity)
            continue;
        if (kv.second.previewFallback && !outResolution.empty())
            continue;
        outResolution = kv.second.resolution.empty() ? std::string("1k") : kv.second.resolution;
        if (!kv.second.previewFallback)
            return true;
    }
    return !outResolution.empty();
}

bool PolyhavenDownloadManager::IsDownloadingOrCompleted(const std::string& slug) const
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    if (m_Shared->CompletedSlugs.count(slug))
        return true;
    for (const auto& r : m_Shared->Active)
        if (r.slug == slug)
            return true;
    for (const auto& c : m_Shared->Completed)
        if (c.slug == slug)
            return true;
    return false;
}

uint32_t PolyhavenDownloadManager::StartHDRIDownload(const std::string& slug,
                                                      const std::string& resolution,
                                                      const std::filesystem::path& assetsRoot)
{
    const std::string res = resolution.empty() ? std::string("1k") : resolution;
    const std::string key = HDRIKey(slug, res);
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);
        if (m_Shared->CompletedSlugs.count(key))
            return 0;
        for (const auto& r : m_Shared->Active)
        {
            if (r.assetType == "hdris" && HDRIKey(r.slug, r.resolution) == key)
                return 0;
        }
        for (const auto& c : m_Shared->Completed)
        {
            if (c.assetType == "hdris" && HDRIKey(c.slug, c.resolution) == key)
                return 0;
        }
    }

    if (!PolyhavenService::FindDownloadedHDRI(slug, assetsRoot, res).empty())
        return 0;

    std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
    float zeroTransform[10] = {};

    uint32_t id = 0;
    std::function<void()> downloadTask;
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);
        m_Shared->CompletedSlugs.erase(key);
        if (m_Shared->ShuttingDown)
            return 0;

        DownloadRequest req{};
        req.downloadId = m_Shared->NextId++;
        req.slug = slug;
        req.assetType = "hdris";
        req.resolution = res;
        req.destDir = destDir;
        std::memcpy(req.savedTransform, zeroTransform, sizeof(req.savedTransform));

        m_Shared->Active.push_back(req);

        id = req.downloadId;
        const std::string slugCopy = req.slug;
        const std::string resCopy = req.resolution;
        const std::filesystem::path dirCopy = req.destDir;

        downloadTask =
            [state = m_Shared, id, slugCopy, resCopy, dirCopy]()
            {
                std::filesystem::path mainFile = PolyhavenService::DownloadHDRI(slugCopy, dirCopy, resCopy);

                CompletedDownload result{};
                result.downloadId = id;
                result.slug = slugCopy;
                result.assetType = "hdris";
                result.resolution = resCopy;
                result.mainFilePath = std::move(mainFile);
                result.success = !result.mainFilePath.empty();
                FinishDownload(state, id, std::move(result));
            };
    }

    LaunchDownloadTask(std::move(downloadTask));
    Logger::Log::Info("PolyhavenDownloadManager: started HDRI download #{} for '{}' {}", id, slug, res);
    return id;
}

uint32_t PolyhavenDownloadManager::StartEarlyDownload(const std::string& slug,
                                                       const std::string& assetType,
                                                       const std::filesystem::path& assetsRoot)
{
    if (IsDownloadingOrCompleted(slug))
        return 0;

    // Check if already fully downloaded in project assets.
    if (!PolyhavenService::FindDownloadedFile(slug, assetsRoot).empty())
        return 0;

    // Download to temp cache — files move to project assets only on actual drop.
    std::filesystem::path cacheDir = PolyhavenService::GetCacheDir() / "downloads" / slug;
    float zeroTransform[10] = {};
    return StartDownload(slug, assetType, cacheDir, ScopedEntity{}, zeroTransform);
}

void PolyhavenDownloadManager::AssignPlaceholder(const std::string& slug,
                                                  ECS::EntityHandle entity,
                                                  ECS::World* world)
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);

    float savedTransform[10] = {};
    if (world)
    {
        if (auto* xf = world->GetComponent<Components::Transform>(entity))
        {
            savedTransform[0] = xf->matrix[12];
            savedTransform[1] = xf->matrix[13];
            savedTransform[2] = xf->matrix[14];
            savedTransform[3] = 0.0f;
            savedTransform[4] = 0.0f;
            savedTransform[5] = 0.0f;
            savedTransform[6] = 1.0f;
            savedTransform[7] = 1.0f;
            savedTransform[8] = 1.0f;
            savedTransform[9] = 1.0f;
        }
    }

    // If the download already completed before the placeholder was assigned,
    // queue a synthetic completion so PollAndProcess replaces it next frame.
    auto completedIt = m_Shared->CompletedSlugs.find(slug);
    if (completedIt == m_Shared->CompletedSlugs.end())
    {
        const std::string hdriPrefix = slug + "|";
        completedIt = std::find_if(
            m_Shared->CompletedSlugs.begin(), m_Shared->CompletedSlugs.end(),
            [&hdriPrefix](const auto& entry)
            {
                return entry.first.size() > hdriPrefix.size() &&
                    entry.first.compare(0, hdriPrefix.size(), hdriPrefix) == 0;
            });
    }
    if (completedIt != m_Shared->CompletedSlugs.end())
    {
        CompletedDownload dl = std::move(completedIt->second);
        dl.Placeholder = ScopedEntity::Capture(entity, TargetWorld());
        std::memcpy(dl.savedTransform, savedTransform, sizeof(dl.savedTransform));
        m_Shared->Completed.push_back(std::move(dl));
        m_Shared->CompletedSlugs.erase(completedIt);
        return;
    }

    // Destroying a superseded placeholder prevents an orphaned "(downloading...)"
    // entity: when the same asset is registered a second time before its first
    // download finishes, PollAndProcess replaces only the latest placeholder, so
    // the earlier one would otherwise linger in the scene forever.
    const ECS::EntityHandle keep = entity;
    auto destroySuperseded = [&](const ScopedEntity& prev)
    {
        if (!world)
            return;
        const ECS::EntityHandle old = prev.Resolve(world);
        if (old.IsValid() && old != keep)
            world->DestroyEntityImmediate(old);
    };

    // Download still in progress — store in the durable map so PollAndProcess
    // picks up the correct entity when the download finishes.
    auto existing = m_Shared->PlaceholderAssignments.find(slug);
    if (existing != m_Shared->PlaceholderAssignments.end())
        destroySuperseded(existing->second.Placeholder);

    PlaceholderAssignment& assignment = m_Shared->PlaceholderAssignments[slug];
    assignment.Placeholder = ScopedEntity::Capture(entity, TargetWorld());
    std::memcpy(assignment.savedTransform, savedTransform, sizeof(assignment.savedTransform));

    // Also update m_Shared->Active and m_Shared->Completed for consistency with status queries.
    for (auto& r : m_Shared->Active)
    {
        if (r.slug == slug)
        {
            destroySuperseded(r.Placeholder);
            r.Placeholder = assignment.Placeholder;
            std::memcpy(r.savedTransform, assignment.savedTransform, sizeof(r.savedTransform));
            return;
        }
    }
    for (auto& c : m_Shared->Completed)
    {
        if (c.slug == slug)
        {
            c.Placeholder = assignment.Placeholder;
            std::memcpy(c.savedTransform, assignment.savedTransform, sizeof(c.savedTransform));
            return;
        }
    }
}

void PolyhavenDownloadManager::AssignHDRIToSkyboxOnComplete(const std::string& slug,
                                                             ECS::EntityHandle entity,
                                                             Editor::EditorChangeNotifications* notifications)
{
    AssignHDRIToSkyboxOnComplete(slug, PolyhavenService::GetPreferredHDRIResolution(), entity, notifications);
}

void PolyhavenDownloadManager::AssignHDRIToSkyboxOnComplete(const std::string& slug,
                                                             const std::string& resolution,
                                                             ECS::EntityHandle entity,
                                                             Editor::EditorChangeNotifications* notifications,
                                                             bool previewFallback)
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);

    const std::string res = resolution.empty() ? std::string("1k") : resolution;
    const std::string key = HDRIKey(slug, res);

    ECS::World* const world = TargetWorld();
    const std::string slugPrefix = slug + "|";
    for (auto it = m_Shared->HDRISkyboxAssignments.begin(); it != m_Shared->HDRISkyboxAssignments.end();)
    {
        const bool sameSkyboxSlug = it->second.Target.Resolve(world) == entity &&
            it->first.size() >= slugPrefix.size() &&
            it->first.compare(0, slugPrefix.size(), slugPrefix) == 0;
        if (!sameSkyboxSlug)
        {
            ++it;
            continue;
        }

        // User-requested resolutions supersede older user-requested resolutions,
        // while preview fallbacks are allowed to coexist with the final target.
        if (!previewFallback && !it->second.previewFallback)
            it = m_Shared->HDRISkyboxAssignments.erase(it);
        else if (previewFallback && it->second.previewFallback && it->first == key)
            it = m_Shared->HDRISkyboxAssignments.erase(it);
        else
            ++it;
    }

    auto completedIt = m_Shared->CompletedSlugs.find(key);
    if (completedIt != m_Shared->CompletedSlugs.end())
    {
        m_Shared->Completed.push_back(std::move(completedIt->second));
        m_Shared->CompletedSlugs.erase(completedIt);
    }

    HDRISkyboxAssignment& assignment = m_Shared->HDRISkyboxAssignments[key];
    assignment.Target = ScopedEntity::Capture(entity, world);
    assignment.resolution = res;
    assignment.notifications = notifications;
    assignment.previewFallback = previewFallback;
}

void PolyhavenDownloadManager::ProcessHDRISkyboxAssignment(const CompletedDownload& dl,
                                                            const HDRISkyboxAssignment& hdriAssign)
{
    if (!dl.success || dl.mainFilePath.empty())
    {
        Logger::Log::Warning("Polyhaven HDRI download failed for '{}'", dl.slug);
        return;
    }

    ECS::World* world = TargetWorld();
    const ECS::EntityHandle skybox = hdriAssign.Target.Resolve(world);
    if (!skybox.IsValid())
    {
        Logger::Log::Warning("Polyhaven: target skybox no longer valid for HDRI '{}'", dl.slug);
        return;
    }
    if (!m_Context || !m_Context->Assets)
    {
        Logger::Log::Warning("Polyhaven: no asset manager for HDRI '{}'", dl.slug);
        return;
    }

    std::filesystem::path mainFilePath = dl.mainFilePath;
    if (!m_Context->AssetsRoot.empty())
    {
        const std::filesystem::path rel = mainFilePath.lexically_relative(m_Context->AssetsRoot);
        if (rel.empty() || rel == "." || rel.generic_string().rfind("..", 0) == 0)
        {
            const std::filesystem::path projectDir = m_Context->AssetsRoot / "Polyhaven" / dl.slug;
            std::filesystem::path moved = PolyhavenService::MoveDownloadToProject(mainFilePath, mainFilePath.parent_path(), projectDir);
            if (!moved.empty())
                mainFilePath = std::move(moved);
        }
    }

    auto& am = *m_Context->Assets;
    GUID guid = am.ResolveAssetGuid(mainFilePath);
    if (guid.IsNull())
    {
        Logger::Log::Warning("Polyhaven: failed to register HDRI '{}'", mainFilePath.string());
        return;
    }

    Components::Skybox sky{};
    if (auto* existing = world->GetComponent<Components::Skybox>(skybox))
        sky = *existing;

    const std::string resString = hdriAssign.resolution.empty()
        ? (dl.resolution.empty() ? std::string("1k") : dl.resolution)
        : hdriAssign.resolution;
    if (hdriAssign.previewFallback && sky.SourceSlug[0] != '\0' &&
        sky.GetSourceSlug() == dl.slug && sky.HDRIAssetGuid[0] != '\0' &&
        HDRIResolutionRank(sky.GetResolution()) >= HDRIResolutionRank(resString))
    {
        Logger::Log::Info("Polyhaven: skipped preview HDRI '{}' {} because skybox already has {}",
                          dl.slug, resString, sky.GetResolution());
        return;
    }

    sky.HDRIIntensity = std::max(0.0f, sky.HDRIIntensity);
    if (sky.HDRIIntensity <= 0.0f)
        sky.HDRIIntensity = 1.0f;

    std::filesystem::path storedPath = mainFilePath;
    if (!m_Context->AssetsRoot.empty())
    {
        const std::filesystem::path rel = mainFilePath.lexically_relative(m_Context->AssetsRoot);
        if (!rel.empty() && rel != "." && rel.generic_string().rfind("..", 0) != 0)
            storedPath = rel;
    }

    const std::string guidString = guid.ToString();
    const std::string pathString = storedPath.generic_string();
    std::memset(sky.HDRIAssetGuid, 0, sizeof(sky.HDRIAssetGuid));
    std::memset(sky.HDRIPath, 0, sizeof(sky.HDRIPath));
    std::memset(sky.Resolution, 0, sizeof(sky.Resolution));
    std::memset(sky.SourceSlug, 0, sizeof(sky.SourceSlug));
    std::strncpy(sky.HDRIAssetGuid, guidString.c_str(), sizeof(sky.HDRIAssetGuid) - 1);
    std::strncpy(sky.HDRIPath, pathString.c_str(), sizeof(sky.HDRIPath) - 1);
    std::strncpy(sky.Resolution, resString.c_str(), sizeof(sky.Resolution) - 1);
    std::strncpy(sky.SourceSlug, dl.slug.c_str(), sizeof(sky.SourceSlug) - 1);
    world->AddComponentImmediate(skybox, sky);

    if (hdriAssign.notifications)
    {
        Editor::EditorChangeNotifications::ComponentChangedEvent e{};
        e.world = world;
        e.entity = skybox;
        e.componentType = ECS::GetComponentTypeId<Components::Skybox>();
        e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
        hdriAssign.notifications->NotifyComponentChanged(e);
        Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
        rebuild.world = world;
        rebuild.entity = skybox;
        rebuild.componentType = ECS::GetComponentTypeId<Components::Skybox>();
        rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
        hdriAssign.notifications->NotifyComponentChanged(rebuild);
    }

    if (m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();

    Logger::Log::Info("Polyhaven: applied HDRI '{}' to skybox entity {}", dl.slug, static_cast<uint32_t>(skybox.index));
}

bool PolyhavenDownloadManager::DownloadAllTextureMaps(const std::string& slug,
                                                         const std::filesystem::path& destDir,
                                                         ECS::EntityHandle entity,
                                                         Editor::UndoRedoService* undoRedo,
                                                         Editor::EditorChangeNotifications* notifications)
{
    uint32_t id = 0;
    std::function<void()> downloadTask;
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);

        m_Shared->CompletedSlugs.erase(slug);

        if (m_Shared->ShuttingDown)
            return false;

        MultiTextureAssignment& multiAssign = m_Shared->MultiTextureAssignments[slug];
        multiAssign.Target = ScopedEntity::Capture(entity, TargetWorld());
        multiAssign.undoRedo = undoRedo;
        multiAssign.notifications = notifications;

        id = m_Shared->NextId++;
        std::string slugCopy = slug;
        std::filesystem::path destDirCopy = destDir;

        downloadTask =
            [state = m_Shared, id, slugCopy, destDirCopy]()
            {
                bool success = PolyhavenService::DownloadTextureMaps(slugCopy, destDirCopy, "all");
                std::filesystem::path mainFile =
                    PolyhavenService::FindDownloadedFile(slugCopy, destDirCopy.parent_path());

                CompletedDownload result{};
                result.downloadId = id;
                result.slug = slugCopy;
                result.assetType = "textures";
                result.mainFilePath = std::move(mainFile);
                result.success = success && !result.mainFilePath.empty();
                FinishDownload(state, id, std::move(result));
            };
    }

    LaunchDownloadTask(std::move(downloadTask));
    Logger::Log::Info("PolyhavenDownloadManager: started multi-map download #{} for '{}'", id, slug);
    return true;
}

void PolyhavenDownloadManager::ProcessMultiTextureAssignment(const CompletedDownload& dl,
                                                              const MultiTextureAssignment& multiAssign)
{
    if (!dl.success || dl.mainFilePath.empty())
    {
        Logger::Log::Warning("Polyhaven multi-map download failed for '{}'", dl.slug);
        return;
    }

    ECS::World* world = TargetWorld();
    const ECS::EntityHandle target = multiAssign.Target.Resolve(world);
    if (!target.IsValid())
    {
        Logger::Log::Warning("Polyhaven: target entity no longer valid for multi-map texture '{}'", dl.slug);
        return;
    }

    if (!m_Context || !m_Context->Assets)
    {
        Logger::Log::Warning("Polyhaven: no asset manager for multi-map texture '{}'", dl.slug);
        return;
    }

    auto& am = *m_Context->Assets;
    std::filesystem::path destDir = dl.mainFilePath.parent_path();

    // Map of Polyhaven map suffixes to material slot names
    struct MapSlotMapping
    {
        const char* fileSuffix;
        const char* slotName;
    };
    static const MapSlotMapping kMapMappings[] = {
        {"_diffuse_", "albedoMap"},
        {"_nor_gl_", "normalMap"},
        {"_rough_", "roughnessMap"},      // Separate roughness (shader combines)
        {"_metallic_", "metallicMap"},      // Separate metallic (shader combines)
        {"_ao_", "aoMap"},
        {"_arm_", "aoMap"}, // AO/Roughness/Metallic combined - use as AO
    };

    int assignedCount = 0;

    // Iterate through all files in the destination directory
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(destDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;

        std::string filename = entry.path().filename().string();

        // Check each mapping
        for (const auto& mapping : kMapMappings)
        {
            if (filename.find(mapping.fileSuffix) != std::string::npos)
            {
                GUID texGuid = am.ResolveAssetGuid(entry.path());
                if (!texGuid.IsNull())
                {
                    // The surface shader applies only when the drop has to create the
                    // material; later maps find the material and ignore it.
                    auto cmd = std::make_unique<Editor::TextureDropOnMeshCommand>(
                        "Assign PBR Texture to Mesh", world, target, texGuid, mapping.slotName,
                        "Surfaces/standard_pbr_extended.glsl",
                        [ctx = m_Context]() { if (ctx && ctx->OnSceneDirty) ctx->OnSceneDirty(); },
                        multiAssign.notifications);
                    if (multiAssign.undoRedo)
                        multiAssign.undoRedo->Execute(std::move(cmd));
                    else
                        cmd->Do();

                    Logger::Log::Info("Polyhaven: assigning {} map '{}' to slot '{}' on entity {}",
                        mapping.fileSuffix, filename, mapping.slotName,
                        static_cast<uint32_t>(target.index));
                    assignedCount++;
                }
                break; // Only match one mapping per file
            }
        }
    }

    if (assignedCount > 0)
    {
        Logger::Log::Info("Polyhaven: assigned {} PBR texture maps for '{}' to entity {}",
            assignedCount, dl.slug, static_cast<uint32_t>(target.index));

        if (m_OnDownloadFinished)
            m_OnDownloadFinished();
    }
    else
    {
        Logger::Log::Warning("Polyhaven: no PBR texture maps assigned for '{}'", dl.slug);
    }
}

void PolyhavenDownloadManager::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);
        m_Shared->ShuttingDown = true;
    }

    // Cancels the queued transfers. No join: on web the main thread must stay
    // responsive for a running transfer's proxied fetch/filesystem ops. SharedState is
    // held by shared_ptr in every task, so an in-flight download writes into a block
    // that outlives this manager and observes ShuttingDown on its next check.
    m_ShutDown = true;
    m_Transfers.reset();
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    m_Shared->Active.clear();
    m_Shared->Completed.clear();
    m_Shared->PlaceholderAssignments.clear();
    m_Shared->CompletedSlugs.clear();
    m_Shared->MultiTextureAssignments.clear();
    m_Shared->HDRISkyboxAssignments.clear();
}

} // namespace GameEngine

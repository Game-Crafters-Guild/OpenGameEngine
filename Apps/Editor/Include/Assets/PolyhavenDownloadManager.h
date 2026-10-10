#pragma once

#include "Assets/DownloadPillOverlay.h"
#include "ECS/Entity.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{

struct EditorContext;
namespace Editor { 
    class EditorChangeNotifications; 
    class UndoRedoService;
}

// Manages background Polyhaven asset downloads triggered by placeholder entity drops.
// Each download is a job of the manager's "Polyhaven" channel (Transfers()) on the job
// system's blocking threads, at most four at once; completions are collected under a
// mutex and polled from the main thread via PollCompleted(). Nothing is joined — see
// Shutdown for why that is safe.
//
// When editor context is provided via SetContext(), PollAndProcess() also handles
// replacing placeholder entities with full model entities on completion.
class PolyhavenDownloadManager
{
public:
    // An entity handle recorded when a download is requested and spent when it
    // completes, potentially many frames later.
    //
    // World::Clear restarts entity versions and fires no per-entity Removed
    // event (ECS/LifecycleEvents.h), so a handle cached before a scene swap,
    // play-mode exit or undo restore still passes World::IsValid while naming
    // an unrelated freshly-loaded entity. Pairing the handle with the world's
    // reset generation at record time is what separates the two. Spend a
    // recorded handle only through Resolve().
    struct ScopedEntity
    {
        ECS::EntityHandle Entity{};
        uint64 Generation = 0;

        static ScopedEntity Capture(ECS::EntityHandle entity, const ECS::World* world)
        {
            ScopedEntity scoped{};
            scoped.Entity = entity;
            scoped.Generation = world ? world->GetLifecycleResetGeneration() : 0;
            return scoped;
        }

        // True once a target was recorded, whether or not it is still spendable —
        // separates "no placeholder was ever assigned" from "the world moved on".
        bool WasRecorded() const { return Entity.IsValid(); }

        // True when the world has been reset since Capture: the recorded entity is
        // already destroyed and the handle now aliases whatever reused its index.
        bool IsStale(const ECS::World* world) const
        {
            return !world || world->GetLifecycleResetGeneration() != Generation;
        }

        // The recorded entity, or an invalid handle if the world was reset since
        // Capture or the entity has since been destroyed.
        ECS::EntityHandle Resolve(const ECS::World* world) const
        {
            if (!WasRecorded() || IsStale(world) || !world->IsValid(Entity))
                return {};
            return Entity;
        }
    };

    struct DownloadRequest
    {
        uint32_t downloadId = 0;
        std::string slug;
        std::string assetType;             // "models", "textures", "hdris"
        std::string resolution;            // HDRI resolution when assetType == "hdris"
        std::filesystem::path destDir;     // target directory inside assets root
        ScopedEntity Placeholder{};
        float savedTransform[10] = {};     // position(3) + rotation(4) + scale(3) — captured at enqueue time
    };

    struct CompletedDownload
    {
        uint32_t downloadId = 0;
        std::string slug;
        std::string assetType;
        std::string resolution;
        std::filesystem::path mainFilePath;  // empty on failure
        ScopedEntity Placeholder{};
        float savedTransform[10] = {};
        bool success = false;
    };

    // Touches no engine service: the editor constructs the manager before the engine
    // initializes. Defined out of line, where JobChannel is complete.
    PolyhavenDownloadManager();
    ~PolyhavenDownloadManager();

    PolyhavenDownloadManager(const PolyhavenDownloadManager&) = delete;
    PolyhavenDownloadManager& operator=(const PolyhavenDownloadManager&) = delete;

    // Start a background download. Returns the assigned downloadId.
    uint32_t StartDownload(const std::string& slug,
                           const std::string& assetType,
                           const std::filesystem::path& destDir,
                           const ScopedEntity& placeholder,
                           const float savedTransform[10]);

    // Convenience: captures the entity's current transform, computes the dest dir
    // from assetsRoot, and starts the download. Returns the assigned downloadId.
    uint32_t StartDownloadForPlaceholder(const std::string& slug,
                                          const std::string& assetType,
                                          const std::filesystem::path& assetsRoot,
                                          ECS::EntityHandle entity,
                                          ECS::World* world);

    // Drain completed downloads into `out`. Call from main thread each frame.
    void PollCompleted(std::vector<CompletedDownload>& out);

    // Provide editor dependencies for automatic placeholder replacement.
    void SetContext(const EditorContext* ctx, Editor::EditorChangeNotifications* notifications);

    // Optional callback fired after each successful download (e.g. refresh asset browser).
    void SetOnDownloadFinished(std::function<void()> cb) { m_OnDownloadFinished = std::move(cb); }
    const std::function<void()>& GetOnDownloadFinished() const { return m_OnDownloadFinished; }

    // Optional callback fired after a placeholder is replaced with a real model entity.
    void SetOnEntityCreated(std::function<void(ECS::EntityHandle)> cb) { m_OnEntityCreated = std::move(cb); }

    // Poll completed downloads, replace placeholders with full models, and fire callbacks.
    // Requires SetContext() to have been called. Call from main thread each frame.
    void PollAndProcess();

    bool HasActiveDownloads() const;
    uint32_t ActiveDownloadCount() const;

    // Scene-view pills for in-flight model downloads; ticked from PollAndProcess().
    DownloadPillOverlay& PillOverlay() { return m_PillOverlay; }

    // Fill `out` with slugs of in-progress downloads.
    void GetActiveDownloadSlugs(std::vector<std::string>& out) const;

    // Fill `out` with {slug, placeholder entity} for each in-flight model-placeholder
    // download whose placeholder still resolves. Drives the download pills for
    // drops made on the scene view or the hierarchy panel.
    void GetActivePlaceholderDownloads(
        const ECS::World* world,
        std::vector<std::pair<std::string, ECS::EntityHandle>>& out) const;

    // File-count download progress for a slug in [0,1]. False when no progress
    // has been reported yet (total unknown until the file list is fetched).
    bool GetDownloadProgress(const std::string& slug, float& outFraction) const;

    // Check if a download is already active or completed for this slug.
    bool IsDownloadingOrCompleted(const std::string& slug) const;

    // Start a background download without a placeholder entity (e.g. during drag).
    // The placeholder entity can be attached later via AssignPlaceholder().
    uint32_t StartEarlyDownload(const std::string& slug,
                                const std::string& assetType,
                                const std::filesystem::path& assetsRoot);

    uint32_t StartHDRIDownload(const std::string& slug,
                               const std::string& resolution,
                               const std::filesystem::path& assetsRoot);

    // Attach a placeholder entity to an already-started download (by slug).
    void AssignPlaceholder(const std::string& slug,
                           ECS::EntityHandle entity,
                           ECS::World* world);

    // Attach a Polyhaven HDRI download to a Skybox entity. When the
    // .hdr arrives, the entity's HDRI slot is populated and the scene is marked dirty.
    void AssignHDRIToSkyboxOnComplete(const std::string& slug,
                                      ECS::EntityHandle entity,
                                      Editor::EditorChangeNotifications* notifications);

    void AssignHDRIToSkyboxOnComplete(const std::string& slug,
                                      const std::string& resolution,
                                      ECS::EntityHandle entity,
                                      Editor::EditorChangeNotifications* notifications,
                                      bool previewFallback = false);

    // True while an HDRI resolution fetch is queued for this skybox entity (inspector UI).
    bool TryGetPendingHdriSkyboxResolution(ECS::EntityHandle entity, std::string& outResolution) const;

    // Download all PBR texture maps for a Polyhaven texture and assign them to material slots.
    // Maps are downloaded to destDir (typically Assets/Polyhaven/<slug>/).
    // Returns true if download was started successfully.
    bool DownloadAllTextureMaps(const std::string& slug,
                                  const std::filesystem::path& destDir,
                                  ECS::EntityHandle entity,
                                  Editor::UndoRedoService* undoRedo,
                                  Editor::EditorChangeNotifications* notifications);

    // The editor's one "Polyhaven" channel: every Polyhaven network transfer (these
    // downloads and the Assets browser's listing, category, thumbnail and download
    // fetches) is a Submit job of it, so at most four transfers run at once and none
    // holds a compute worker. Created on first use on the engine's job system (the
    // manager itself is constructed before the engine initializes); null once Shutdown
    // has run. Main thread only.
    JobSystem::JobChannel* Transfers();

    // Cancels the queued transfers and releases the channel; running ones finish on
    // their own. Nothing is joined: jobs keep a SharedState ref and no-op their
    // completion write if shutdown already ran — the main thread must not wait on a
    // transfer that proxies fetch/FS through it on web.
    void Shutdown();

private:
    struct PlaceholderAssignment
    {
        ScopedEntity Placeholder{};
        float savedTransform[10] = {};
    };

    // Multi-map texture assignment for PBR textures (albedo, normal, roughness, metallic, AO, etc.)
    struct MultiTextureAssignment
    {
        ScopedEntity Target{};
        Editor::UndoRedoService* undoRedo = nullptr;
        Editor::EditorChangeNotifications* notifications = nullptr;
    };

    struct HDRISkyboxAssignment
    {
        ScopedEntity Target{};
        std::string resolution;
        Editor::EditorChangeNotifications* notifications = nullptr;
        bool previewFallback = false;
    };

    void ReplacePlaceholder(const CompletedDownload& dl);
    void ProcessMultiTextureAssignment(const CompletedDownload& dl, const MultiTextureAssignment& multiAssign);
    void ProcessHDRISkyboxAssignment(const CompletedDownload& dl, const HDRISkyboxAssignment& hdriAssign);

    // Heap block jobs capture. Shutdown cannot join JobSystem work on web
    // (fetch/FS proxy through the main thread), so the block outlives `this`
    // until the last in-flight lambda drops its ref.
    struct SharedState
    {
        mutable std::mutex Mutex;
        uint32_t NextId = 1;
        std::vector<DownloadRequest> Active;
        std::vector<CompletedDownload> Completed;
        std::unordered_map<std::string, PlaceholderAssignment> PlaceholderAssignments;
        std::unordered_map<std::string, CompletedDownload> CompletedSlugs;
        std::unordered_map<std::string, MultiTextureAssignment> MultiTextureAssignments;
        std::unordered_map<std::string, HDRISkyboxAssignment> HDRISkyboxAssignments;
        // slug -> (filesDone, filesTotal), written from the download thread.
        std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> Progress;
        bool ShuttingDown = false;
    };

    static void FinishDownload(const std::shared_ptr<SharedState>& state,
                               uint32_t id,
                               CompletedDownload&& result);
    void LaunchDownloadTask(std::function<void()> task);

    std::shared_ptr<SharedState> m_Shared = std::make_shared<SharedState>();
    std::unique_ptr<JobSystem::JobChannel> m_Transfers; // "Polyhaven" (cap 4); created by Transfers()
    bool m_ShutDown = false;                            // main-thread only
    DownloadPillOverlay m_PillOverlay{*this};           // main-thread only
    std::function<void()> m_OnDownloadFinished;        // main-thread only
    std::function<void(ECS::EntityHandle)> m_OnEntityCreated; // main-thread only
    // Slugs whose model or texture load has finished since ReplacePlaceholder deferred them,
    // so the retry treats an asset still missing as a failed load. Main-thread only.
    std::unordered_set<std::string> m_LoadsFinished;
    const EditorContext* m_Context = nullptr;           // not owned
    Editor::EditorChangeNotifications* m_Notifications = nullptr; // not owned
};

} // namespace GameEngine

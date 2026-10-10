#include "Scene/SceneDocumentManager.h"

#include "Diagnostics/MainThreadHangWatchdog.h"
#include "Scene/DefaultSceneEntities.h"

#include "Assets/AssetManager.h"
#include "Assets/HlodBakeDriver.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/DeviceLostEcsRecovery.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "Scene/SceneBackupRecovery.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneEngineEmbedMaterializer.h"
#include "Terrain/SphereSculptSaver.h"
#include "Terrain/TerrainBakeCacheSave.h"
#include "Terrain/TerrainMaterialLibraryMint.h"
#include "Terrain/TerrainZonePayloadSaver.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainGrass/Scene/TerrainGrassSceneSchemas.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include "ECS/World.h"

#include <format>

namespace GameEngine::Editor
{
namespace
{
class EditorSceneAssetResolver final : public Scene::ISceneAssetResolver
{
  public:
    EditorSceneAssetResolver(AssetManager& assetManager, std::filesystem::path assetRoot)
        : m_AssetManager(assetManager), m_Registry(assetManager.GetRegistry()),
          m_AssetRoot(std::move(assetRoot))
    {
    }

    std::filesystem::path GetAssetRoot() const override { return m_AssetRoot; }
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const override
    {
        return m_AssetManager.ResolveAssetPath(authoredPath);
    }
    GUID ResolveGuid(const GUID& guid) const override { return m_Registry.ResolveGuid(guid); }
    GUID GetOrCreateAssetGuid(const std::filesystem::path& absolutePath) override
    {
        const auto result = m_Registry.RegisterAssetByPath(absolutePath);
        return result.IsOk() ? result.Value() : GUID::Null();
    }

    bool TryGetPathAndType(const GUID& guid, std::filesystem::path& outPath, AssetType& outType) const override
    {
        AssetMetadata md{};
        if (!m_Registry.TryGetAssetMetadata(guid, md))
            return false;
        outPath = md.Path;
        outType = md.Type;
        return true;
    }

    bool TryGetGuidAndType(const std::filesystem::path& absolutePath, GUID& outGuid, AssetType& outType) const override
    {
        AssetMetadata md{};
        if (!m_Registry.TryGetAssetMetadata(absolutePath, md))
            return false;
        outGuid = md.Guid;
        outType = md.Type;
        return true;
    }

  private:
    AssetManager& m_AssetManager;
    AssetRegistry& m_Registry;
    std::filesystem::path m_AssetRoot;
};
} // namespace

SceneDocumentManager::~SceneDocumentManager()
{
    Unsubscribe();
}

void SceneDocumentManager::SetWorld(ECS::World* world)
{
    m_World = world;
}

std::optional<std::string> SceneDocumentManager::GetDisplaySceneName() const
{
    if (m_DisplaySceneName.has_value() && !m_DisplaySceneName->empty())
        return m_DisplaySceneName;
    if (m_ScenePath.has_value() && !m_ScenePath->empty())
        return m_ScenePath->filename().string();
    return std::nullopt;
}

void SceneDocumentManager::SetChangeNotifications(EditorChangeNotifications* notifications)
{
    if (m_Notifications == notifications)
        return;
    Unsubscribe();
    m_Notifications = notifications;
    Subscribe();
}

void SceneDocumentManager::MarkDirty()
{
    if (!m_Dirty)
        Logger::Log::Debug("SceneDocumentManager: scene marked dirty");
    m_Dirty = true;
}

void SceneDocumentManager::MarkClean()
{
    if (m_Dirty)
        Logger::Log::Debug("SceneDocumentManager: scene marked clean");
    m_Dirty = false;
    m_BaselineSnapshot = CaptureWorldSnapshot();
}

void SceneDocumentManager::SetHierarchyUiCaptureForSave(std::function<Scene::SceneHierarchyUi()> capture)
{
    m_HierarchyUiCaptureForSave = std::move(capture);
}

std::optional<Scene::SceneHierarchyUiFromFile> SceneDocumentManager::ConsumeHierarchyUiFromLastDocumentLoad()
{
    std::optional<Scene::SceneHierarchyUiFromFile> out = std::move(m_PendingHierarchyUiFromLoad);
    m_PendingHierarchyUiFromLoad.reset();
    return out;
}

void SceneDocumentManager::SetEditorCameraCaptureForSave(std::function<Scene::SceneEditorCamera()> capture)
{
    m_EditorCameraCaptureForSave = std::move(capture);
}

std::optional<Scene::SceneEditorCameraFromFile> SceneDocumentManager::ConsumeEditorCameraFromLastDocumentLoad()
{
    std::optional<Scene::SceneEditorCameraFromFile> out = std::move(m_PendingEditorCameraFromLoad);
    m_PendingEditorCameraFromLoad.reset();
    return out;
}

void SceneDocumentManager::SetSceneScopedGeneratorRelease(std::function<void()> release)
{
    m_SceneScopedGeneratorRelease = std::move(release);
}

void SceneDocumentManager::RecordLoadFailure(const std::filesystem::path& documentPath,
                                             const std::filesystem::path& contentPath)
{
    const auto& err = Scene::GetLastSceneIOError();

    SceneLoadFailure failure{};
    failure.DocumentPath = documentPath;
    failure.Message = err.message;
    failure.ErrorFile = err.file.empty() ? contentPath : err.file;
    failure.ErrorLine = err.line;
    failure.WorldCleared = err.worldCleared;
    failure.EntitiesInWorld = m_World ? m_World->GetEntityCount() : 0;

    const std::string where =
        failure.ErrorFile.empty()
            ? failure.DocumentPath.string()
            : failure.ErrorFile.string() + ":" + std::to_string(failure.ErrorLine);
    // The log lines below end the reason with their own period, so one the
    // loader's sentence already carries is dropped rather than doubled.
    std::string why = failure.Message.empty() ? "no error reported" : failure.Message;
    if (why.ends_with('.'))
        why.pop_back();

    if (failure.WorldCleared)
    {
        // The document cannot go on naming a scene that is not in the world:
        // the window title would keep showing it, and Save would write this
        // partial world straight over that file.
        m_ScenePath.reset();
        m_DisplaySceneName = failure.DocumentPath.filename().string() + " (LOAD FAILED)";
        m_PartialLoadDocument = failure.DocumentPath;

        // A degraded record describes the document the clear just destroyed. Left standing it
        // would report a scene that is no longer open, alongside the failure for the one that
        // replaced it — two different files described as the current state at once. A failure
        // that did NOT clear leaves its degraded document open and still degraded, so the record
        // only goes with the world.
        m_LastLoadDegraded.reset();

        Logger::Log::Error("SceneDocument: '{}' failed to load at {}: {}. The world was already cleared and "
                           "now holds {} from that file, a partial load and not a scene. The document has no "
                           "path, so Save asks for a new file name until a scene opens.",
                           failure.DocumentPath.string(), where, why,
                           FormatCount(failure.EntitiesInWorld, "entity", "entities"));
    }
    else
    {
        // Not cleared: a pre-Clear replace failure (world untouched) or an
        // additive one (the world survives, but this file's entities may have
        // landed before the abort). The world is described as it was before
        // this open: the document, the partial load an earlier failure left,
        // or an untitled scene.
        const std::string entities = FormatCount(failure.EntitiesInWorld, "entity", "entities");
        std::string state;
        if (m_ScenePath)
            state = "'" + m_ScenePath->filename().string() + "' stays open, and the world holds " + entities + ".";
        else if (m_PartialLoadDocument)
            state = "The world still holds " + entities + " from the partial load of '" +
                    m_PartialLoadDocument->filename().string() + "'.";
        else
            state = "The untitled scene stays open, and the world holds " + entities + ".";
        Logger::Log::Error("SceneDocument: '{}' failed to load at {}: {}. {}", failure.DocumentPath.string(),
                           where, why, state);
    }

    m_LastLoadFailure = std::move(failure);
}

void SceneDocumentManager::RecordLoadDegradation(const std::filesystem::path& documentPath,
                                                 Scene::SceneLoadDegradation census)
{
    if (!census.IsDegraded())
    {
        m_LastLoadDegraded.reset();
        return;
    }

    const std::size_t dropped = census.DroppedCount();
    Logger::Log::Error("SceneDocument: '{}' loaded DEGRADED — {} assignment(s) could not be applied "
                       "by this build ({} preserved, {} would be lost). The scene is open and usable; "
                       "Save will not overwrite the file without explicit confirmation.",
                       documentPath.string(), census.skips.size(), census.skips.size() - dropped,
                       dropped);

    SceneLoadDegraded record{};
    record.DocumentPath = documentPath;
    record.Census = std::move(census);
    m_LastLoadDegraded = std::move(record);
    RefreshDisplayNameForDegradation();
}

void SceneDocumentManager::RefreshDisplayNameForDegradation()
{
    // The title is the one thing the user cannot miss, and a scene that is quietly missing part of
    // itself looks exactly like a healthy one. Mark it for as long as the record stands — the
    // decoration is display-only and never reaches the file or the document path.
    if (!m_ScenePath.has_value())
        return;
    const std::string base = m_ScenePath->filename().string();
    m_DisplaySceneName = m_LastLoadDegraded.has_value() ? base + " (PARTIAL LOAD)" : base;
}

std::optional<SceneLoadDegraded> SceneDocumentManager::GetOutstandingLoadDegraded() const
{
    if (!m_LastLoadDegraded.has_value())
        return std::nullopt;
    // No world to ask means no way to know what has retired. Report the load-time record rather
    // than an empty one: the failure mode of guessing "nothing is outstanding" is a surface that
    // calls a degraded scene healthy, which is the bug this whole change is about.
    if (!m_World)
        return m_LastLoadDegraded;

    SceneLoadDegraded outstanding{};
    outstanding.DocumentPath = m_LastLoadDegraded->DocumentPath;
    for (const Scene::SceneLoadSkip& s : m_LastLoadDegraded->Census.skips)
    {
        if (Scene::SkipIsOutstanding(*m_World, s))
            outstanding.Census.skips.push_back(s);
    }
    if (outstanding.Census.skips.empty())
        return std::nullopt;
    return outstanding;
}

std::size_t SceneDocumentManager::OutstandingDegradedSkipCount() const
{
    if (!m_LastLoadDegraded.has_value())
        return 0;
    // Same conservative fallback as above: with no world, every skip stays outstanding, so the
    // guard keeps the answer the load gave it instead of being dropped by an unanswerable question.
    if (!m_World)
        return m_LastLoadDegraded->SkippedCount();
    return Scene::OutstandingSkipCount(*m_World, m_LastLoadDegraded->Census);
}

bool SceneDocumentManager::WouldRefuseDegradedOverwrite(const std::filesystem::path& targetPath,
                                                        DegradedSavePolicy policy) const
{
    // Counted, not merely present: once the user has discarded every preserved override the guard
    // is protecting nothing, and refusing the save would be nagging about a decision they made.
    // Never-preserved skips never retire, so a scene that would genuinely lose data stays guarded.
    if (OutstandingDegradedSkipCount() == 0)
        return false;
    if (policy == DegradedSavePolicy::SaveAnyway)
        return false;
    // Only the file this document was READ from is irreplaceable. Any other target is a copy.
    return m_ScenePath.has_value() &&
           m_ScenePath->lexically_normal() == targetPath.lexically_normal();
}

std::string SceneDocumentManager::DescribeDegradedRefusal(const std::filesystem::path& targetPath) const
{
    // The OUTSTANDING counts, not the load-time ones: an override the user has already discarded is
    // not something this refusal protects, so counting it would overstate what is at stake.
    // WouldRefuseDegradedOverwrite gates this call on outstanding > 0, so the view is non-empty.
    const std::optional<SceneLoadDegraded> outstanding = GetOutstandingLoadDegraded();
    return std::format("refusing to save over '{}' — it loaded with {} assignment(s) "
                       "this build could not read, and saving would write {} of them back as "
                       "defaults. Save As to another file, or confirm explicitly to overwrite.",
                       targetPath.string(),
                       outstanding.has_value() ? outstanding->SkippedCount() : 0,
                       outstanding.has_value() ? outstanding->DroppedCount() : 0);
}

bool SceneDocumentManager::OpenSceneReplace(const std::filesystem::path& scenePath)
{
    return OpenSceneFromFile(scenePath, scenePath, /*openAsDirty*/ false);
}

bool SceneDocumentManager::OpenSceneRecovered(const std::filesystem::path& backupScenePath,
                                              const std::filesystem::path& scenePath)
{
    // The recovered state exists only in the backup until the user saves it, so
    // the document opens dirty. MarkDirty is deferred to FinishSceneBuild (the
    // resolve pump runs across frames and MarkClean lands at its tail).
    if (!OpenSceneFromFile(backupScenePath, scenePath, /*openAsDirty*/ true))
        return false;
    Logger::Log::Info("SceneDocument: recovering '{}' from '{}'", scenePath.string(),
                      backupScenePath.filename().string());
    return true;
}

bool SceneDocumentManager::OpenSceneFromFile(const std::filesystem::path& contentPath,
                                             const std::filesystem::path& documentPath,
                                             bool openAsDirty)
{
    if (!m_World)
        return false;

    auto* rs = EngineCore::GetInstance().GetRenderServices();

    Logger::Log::Info("SceneDocument: loading (replace) '{}'", contentPath.string());

    // Everything scoped to the outgoing scene dies here, before LoadSceneFromFile
    // reaches World::Clear.
    ReleaseSceneScopedState(rs);

    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    Scene::SceneHierarchyUiFromFile hierarchyFromFile{};
    opts.outHierarchyUiFromFile = &hierarchyFromFile;
    Scene::SceneEditorCameraFromFile editorCameraFromFile{};
    opts.outEditorCameraFromFile = &editorCameraFromFile;
    Scene::SceneLoadTimings timings{};
    opts.outTimings = &timings;
    Scene::SceneLoadDegradation degradation{};
    opts.outDegradation = &degradation;

    auto& am = EngineCore::GetInstance().GetAssetManager();
    EditorSceneAssetResolver resolver(am, EngineCore::GetInstance().GetResolvedAssetRoot());
    opts.assetResolver = &resolver;
    opts.assetRootOverride = resolver.GetAssetRoot();
    opts.projectRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Scene::SceneEngineEmbedMaterializer embedMat(am);
    opts.embedMaterializer = &embedMat;

    const auto tLoad0 = std::chrono::steady_clock::now();
    Editor::HangWatchdogPhase watchdogPhase("SceneLoad.ParseInstantiate");
    const bool ok = Scene::LoadSceneFromFile(*m_World, contentPath, opts);
    const double loadMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tLoad0)
                              .count();
    if (!ok)
    {
        // Two different worlds can be live here. Read, parse, include expansion
        // and validation all run BEFORE World::Clear, so those failures leave the
        // outgoing scene intact. Failures the loader cannot continue past — an
        // unreadable blueprint, a bad remove directive, entity creation — happen
        // AFTER it, and leave a partially-applied incoming scene in the world.
        // A component or field that will not apply is NOT one of these: it is
        // collected into the degradation census and skipped, and the load
        // succeeds.
        // Either way the proxies were retired above, so rebuild them against
        // whatever the user is now looking at — while m_ScenePath still names the
        // outgoing scene, because that is the scene those proxies belong to.
        // RecordLoadFailure drops that name afterwards for the cleared case.
        //
        // WHAT TO DO with a half-loaded world (leave it, empty it, reload the
        // outgoing scene) is an open design question. This function only refuses
        // to pretend it did not happen.
        if (rs && m_ScenePath.has_value())
            ReconcileHlodForScene(*m_ScenePath, *rs);

        RecordLoadFailure(documentPath, contentPath);
        return false;
    }

    m_LastLoadFailure.reset();
    m_PartialLoadDocument.reset();
    m_ScenePath = documentPath;
    m_DisplaySceneName = documentPath.filename().string();
    // After the display name, which this may decorate.
    RecordLoadDegradation(documentPath, std::move(degradation));
    m_ZoneStagingLedger.StagedDataVersions.clear();
    m_ZoneStagingLedger.StagedSphereSculptVersion = 0;

    m_PendingHierarchyUiFromLoad = hierarchyFromFile;
    m_PendingEditorCameraFromLoad = editorCameraFromFile;

    // The scene FILE is loaded (all entities exist); the per-entity GPU mesh/
    // material resolve is the expensive part (~20s for a 21k-entity scene). Defer
    // it to a per-frame budgeted pump so the editor keeps presenting frames and
    // entities materialize progressively (RenderExtractionSystem safely skips the
    // not-yet-resolved). FinishSceneBuild runs the one-shot tail (HLOD reconcile,
    // structure notify, MarkClean) once the pump drains — see PumpSceneBuild.
    m_BuildDocumentPath = documentPath;
    m_BuildOpenAsDirty = openAsDirty;
    m_BuildActive = true;
    m_BuildStartTime = std::chrono::steady_clock::now();
    m_BuildPumpCpuMs = 0.0;

    if (rs)
        m_BuildPump.Begin(*m_World, *rs);

    Logger::Log::Info("SceneDocument: loaded file '{}' in {:.0f}ms main thread "
                      "(read {:.1f} / parse {:.1f} / validate {:.1f} / clear {:.1f} / "
                      "instantiate {:.1f} ms); resolving {} entities across frames",
                      documentPath.string(), loadMs, timings.ReadMs, timings.ParseMs,
                      timings.ValidateMs, timings.ClearMs, timings.InstantiateMs,
                      m_BuildPump.TotalItems());

    // A scene with nothing to resolve (or a headless run with no renderer)
    // completes immediately — run the tail this frame instead of waiting a pump
    // tick, preserving the synchronous-open contract for those cases.
    if (!rs || m_BuildPump.IsComplete())
        FinishSceneBuild(rs);

    return true;
}

void SceneDocumentManager::PumpSceneBuild(std::chrono::milliseconds budget)
{
    if (!m_BuildActive)
        return;

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
    {
        // Renderer vanished mid-build (device teardown): finish the bookkeeping
        // so the document doesn't wedge in a permanently-building state.
        FinishSceneBuild(nullptr);
        return;
    }

    // The pump uploads GPU mesh/texture resources through persistently-mapped
    // pools. An in-place device rebuild frees those pools (AwaitingReprovision)
    // until the Q6 recovery re-provisions them, so stepping meanwhile is an
    // upload-time use-after-free. Hold the build — don't step, don't finish —
    // until the device is Healthy again; the render loop is suppressed while not
    // Healthy too, so waiting costs no visible frames.
    if (auto* device = rs->GetDevice();
        device && device->GetDeviceHealth() != Rendering::DeviceHealth::Healthy)
        return;

    const auto tStep = std::chrono::steady_clock::now();
    m_BuildPump.StepBudgeted(*m_World, *rs, budget);
    m_BuildPumpCpuMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStep).count();

    if (m_BuildPump.IsComplete())
        FinishSceneBuild(rs);
}

void SceneDocumentManager::DrainSceneBuild()
{
    if (!m_BuildActive)
        return;

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
    {
        FinishSceneBuild(nullptr);
        return;
    }

    // Same device-health gate as PumpSceneBuild: draining uploads through mapped
    // pools, unsafe while AwaitingReprovision. If the device isn't Healthy we
    // can't drain now — leave the build active; the caller proceeds against a
    // still-partial world (rare device-lost-mid-load; the Q6 recovery re-resolves
    // on the next Healthy tick).
    if (auto* device = rs->GetDevice();
        device && device->GetDeviceHealth() != Rendering::DeviceHealth::Healthy)
        return;

    Editor::HangWatchdogPhase watchdogPhase("SceneBuild.DrainNow");
    const auto tDrain = std::chrono::steady_clock::now();
    m_BuildPump.DrainNow(*m_World, *rs);
    m_BuildPumpCpuMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tDrain).count();
    FinishSceneBuild(rs);
}

void SceneDocumentManager::FinishSceneBuild(Engine::Renderer::RenderServices* renderServices)
{
    // The tail runs as one main-thread block (HLOD reconcile, structure-change
    // fan-out incl. MissingAssetTracker rescan, hierarchy rebuild) while the
    // frozen "Loading scene N/M" overlay still shows the pump's last counts.
    Editor::HangWatchdogPhase watchdogPhase("SceneBuild.FinishTail");

    const std::filesystem::path documentPath =
        m_BuildDocumentPath.value_or(m_ScenePath.value_or(std::filesystem::path{}));

    if (renderServices)
    {
        // HLOD: reconcile this scene's baked proxy clusters (if any) against the
        // now fully-resolved entities so far static clusters can switch to their
        // merged proxy at range. Keyed by the document identity — a recovered open
        // must resolve the real scene's bake, not one for the backup file.
        ReconcileHlodForScene(documentPath, *renderServices);
        // Sync the recovery poll baseline so a rebuild that preceded this load does
        // not double-reconcile the freshly-reconciled proxies (see PollDeviceRebuild...).
        if (auto* device = renderServices->GetDevice())
            m_LastDeviceRebuildGeneration = device->GetDeviceRebuildGeneration();
    }

    if (m_Notifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        e.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

    // After UI refresh (same event currently calls MarkDirty via our subscription),
    // reset to clean + baseline. A recovery open then re-dirties: its state exists
    // only in the backup until the user saves.
    MarkClean();
    const bool openedDirty = m_BuildOpenAsDirty;
    if (openedDirty)
        MarkDirty();

    const double buildS = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - m_BuildStartTime)
                              .count();
    // Wall-clock spans the frames the budgeted pump waited through; the CPU
    // figure is the work itself. A delta-based reload could only ever skip the
    // second number, so quoting the first would overstate its prize.
    Logger::Log::Info("SceneDocument: loaded '{}' (dirty={}); resolve pump drained "
                      "{} items in {:.1f}s wall, {:.0f}ms main-thread CPU",
                      documentPath.string(), openedDirty ? "true" : "false",
                      m_BuildPump.ProcessedItems(), buildS, m_BuildPumpCpuMs);

    m_BuildPump.Reset();
    m_BuildActive = false;
    m_BuildDocumentPath.reset();
    m_BuildOpenAsDirty = false;
}

void SceneDocumentManager::CancelSceneBuild()
{
    // Contract: this MUST run before any World::Clear / scene swap. The pump holds
    // ECS::EntityHandles into the current world; World::Clear restarts entity
    // versions WITHOUT changing the world id, so a leaked handle would silently
    // alias a freshly-seeded entity (an ABA the generational IsValid check cannot
    // catch). All swap sites (OpenSceneFromFile, LoadSceneAdditive, NewSceneUntitled)
    // call this first.
    m_BuildPump.Reset();
    m_BuildActive = false;
    m_BuildDocumentPath.reset();
    m_BuildOpenAsDirty = false;
}

void SceneDocumentManager::ReleaseSceneScopedState(Engine::Renderer::RenderServices* renderServices)
{
    // The pump holds EntityHandles into the outgoing world.
    CancelSceneBuild();

    // Proxy meshes have no release path besides Retire (C7), and the cluster's
    // proxy handles stop meaning anything the moment the world is cleared.
    if (renderServices && m_World)
        renderServices->GetHlodRuntime().Retire(*m_World, renderServices->GetMeshGPURegistry());

    // The spline generators cache handles to their generated entities and retire
    // them on a later sweep, keyed off the spline entity still being present. An
    // incoming scene without that spline makes the whole cached set unvisited, so
    // the sweep retires it — against the NEW world, where World::Clear's version
    // restart has made every stale handle validate against the incoming entity
    // that recycled its index. Same hazard the HLOD retire above answers.
    //
    // This site covers the clears that pass through here, and is NOT the whole
    // guarantee: World::DeserializeWorld clears the world too (play-mode exit,
    // undo snapshot restore) and never reaches this function, so the generators
    // additionally drop their caches when the lifecycle reset generation moves.
    if (m_SceneScopedGeneratorRelease)
        m_SceneScopedGeneratorRelease();

    // A replace-open discards the previous session's zone payload edits, so the
    // incoming scene's zones re-resolve from their .tzone files. A leaked
    // resident payload would keep NeedsSave forever (phantom-dirty document) and
    // the next explicit Save would persist strokes the user chose to discard.
    // The sphere sculpt store discards the same way — the incoming planet
    // re-restores from its .tsculpt via EnsureSphereSculptLoaded.
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
    {
        terrain->ClearZonePayloads();
        terrain->ResetPlanetSculpt();
    }
}

bool SceneDocumentManager::GetSceneBuildProgress(uint64_t& outProcessed, uint64_t& outTotal) const
{
    if (!m_BuildActive)
        return false;
    outProcessed = static_cast<uint64_t>(m_BuildPump.ProcessedItems());
    outTotal = static_cast<uint64_t>(m_BuildPump.TotalItems());
    return true;
}

void SceneDocumentManager::ReconcileHlodForScene(const std::filesystem::path& scenePath,
                                                 Engine::Renderer::RenderServices& renderServices)
{
    if (!m_World)
        return;

    // WorldTransform is derived (not serialized); on a fresh load no frame has
    // ticked yet, so seed it with one hierarchy pass first — the reconcile gathers
    // member world positions and recomputes the C3 staleness hash from them, and
    // they must match the values the bake stored.
    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    Hlod::HlodRuntime& hlod = renderServices.GetHlodRuntime();
    const std::filesystem::path hlodFile = Hlod::ResolveHlodCacheFile(assetManager, scenePath);
    if (!hlodFile.empty() && std::filesystem::exists(hlodFile))
    {
        Engine::Renderer::TransformHierarchySystem hierarchy;
        hierarchy.Update(*m_World, 0.0f);
        m_World->ProcessCommands();

        Hlod::ModelResolver modelResolver = [&assetManager](const GUID& guid) -> const ModelAsset* {
            return dynamic_cast<const ModelAsset*>(assetManager.GetAsset(guid).get());
        };
        hlod.ReconcileScene(*m_World, renderServices.GetMeshGPURegistry(), hlodFile,
                            scenePath.stem().string(), modelResolver);
    }
    else
    {
        // No bake for this scene: release any prior scene's proxy meshes.
        hlod.Retire(*m_World, renderServices.GetMeshGPURegistry());
    }
}

void SceneDocumentManager::PollDeviceRebuildAndRecoverHlod()
{
    if (!m_World || !m_ScenePath.has_value())
        return;

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* device = rs->GetDevice();
    if (!device)
        return;

    // Defer until the device is Healthy again — NOT merely AwaitingReprovision.
    // ReconcileScene below calls RegisterSubmesh -> UploadMesh, which writes through
    // MeshGPURegistry's persistently-mapped VMA pools. A rebuild frees those pools and
    // RenderServices::OnDeviceRebuilt does not re-provision the registry — slice 4 does,
    // before rendering resumes. Running the reconcile during AwaitingReprovision is an
    // upload-time use-after-free. The generation poll is preserved inside the helper
    // (it does not consume the generation while suppressed), so recovery fires on the
    // first Healthy tick after re-provision. Extraction-phase pollers (e.g. the eztree
    // package's system) need no equivalent gate: RenderingLoop suppresses that phase
    // until Healthy.
    if (!Engine::Renderer::ShouldRunHealthyGatedDeviceRecovery(
            device->GetDeviceHealth(), device->GetDeviceRebuildGeneration(),
            m_LastDeviceRebuildGeneration))
        return;

    Logger::Log::Warning(
        "Q6 slice 3b: re-reconciling HLOD proxies after device rebuild (scene='{}')",
        m_ScenePath->filename().string());
    ReconcileHlodForScene(*m_ScenePath, *rs);
}

bool SceneDocumentManager::LoadSceneAdditive(const std::filesystem::path& scenePath)
{
    if (!m_World)
        return false;

    // Additive load resolves synchronously below (ResolveModelMeshRenderers picks
    // up everything still unresolved); drop any deferred replace-open build first
    // so its pump doesn't keep touching a world this load is mutating underneath.
    CancelSceneBuild();

    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Additive;

    auto& am = EngineCore::GetInstance().GetAssetManager();
    EditorSceneAssetResolver resolver(am, EngineCore::GetInstance().GetResolvedAssetRoot());
    opts.assetResolver = &resolver;
    opts.assetRootOverride = resolver.GetAssetRoot();
    opts.projectRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Scene::SceneEngineEmbedMaterializer embedMat(am);
    opts.embedMaterializer = &embedMat;

    Scene::SceneLoadDegradation degradation{};
    opts.outDegradation = &degradation;

    const bool ok = Scene::LoadSceneFromFile(*m_World, scenePath, opts);
    if (!ok)
    {
        // An additive load never clears, so the open document survives — but the
        // entities this file did create before aborting are still in the world.
        RecordLoadFailure(scenePath, scenePath);
        return false;
    }

    // Additive entities join the world, so they join the next Save — anything this file could not
    // apply is data the document would write back stripped, exactly like the replace case. Merge it
    // in rather than replacing: the record guards what is in the WORLD, not what the last call read.
    // A clean additive therefore leaves a standing record alone; it recovers nothing.
    if (degradation.IsDegraded())
    {
        SceneLoadDegraded merged =
            m_LastLoadDegraded.value_or(SceneLoadDegraded{m_ScenePath.value_or(scenePath), {}});
        merged.Census.skips.insert(merged.Census.skips.end(),
                                   std::make_move_iterator(degradation.skips.begin()),
                                   std::make_move_iterator(degradation.skips.end()));
        Logger::Log::Error("SceneDocument: additive load of '{}' could not apply {} assignment(s); "
                           "the open document now carries {} in total and Save is guarded.",
                           scenePath.string(), degradation.skips.size(), merged.SkippedCount());
        m_LastLoadDegraded = std::move(merged);
        RefreshDisplayNameForDegradation();
    }

    // An additive load restores no outgoing scene and adopts no document
    // identity, so succeeding on top of a cleared-world failure recovers
    // nothing — it only adds entities to the partial load already there. The
    // record and the "(LOAD FAILED)" name are the only things saying so, and
    // clearing them here would put back the impersonation the failure ended.
    if (!m_LastLoadFailure.has_value() || !m_LastLoadFailure->WorldCleared)
    {
        m_LastLoadFailure.reset();
        m_DisplaySceneName = scenePath.filename().string();
    }

    // Resolve model asset GUIDs to GPU mesh handles for entities loaded from the scene file.
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        Engine::Renderer::ResolveModelMeshRenderers(*m_World, *rs);
        // Register any standalone .material assets referenced by entities so the
        // renderer doesn't skip meshes whose material isn't yet in the registry
        // (model-embedded materials are already registered above).
        Engine::Renderer::ResolveStandaloneMaterials(*m_World, *rs);
    }

    m_PendingHierarchyUiFromLoad.reset();
    m_PendingEditorCameraFromLoad.reset();

    MarkDirty();

    if (m_Notifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        e.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

    return true;
}

void SceneDocumentManager::NewSceneUntitled(Engine::Renderer::RenderServices* renderServices)
{
    ReleaseSceneScopedState(renderServices);

    if (m_World)
    {
        m_World->Clear();
        m_World->ProcessCommands();
        SeedDefaultSceneEntities(*m_World, renderServices);
    }
    m_ScenePath.reset();
    m_DisplaySceneName.reset();
    m_LastLoadFailure.reset(); // this world is now a new scene, not a failed one
    m_PartialLoadDocument.reset();
    m_LastLoadDegraded.reset();
    m_ZoneStagingLedger.StagedDataVersions.clear();
    m_ZoneStagingLedger.StagedSphereSculptVersion = 0;

    m_PendingHierarchyUiFromLoad = Scene::SceneHierarchyUiFromFile{};
    m_PendingEditorCameraFromLoad = Scene::SceneEditorCameraFromFile{};

    if (m_Notifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        e.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

    MarkClean();
}

void SceneDocumentManager::SetPlayModeProbe(std::function<bool()> isNotEditing)
{
    m_PlayModeProbe = std::move(isNotEditing);
}

bool SceneDocumentManager::IsSaveBlockedByPlayMode() const
{
    return m_PlayModeProbe && m_PlayModeProbe();
}

std::string SceneDocumentManager::DescribeSaveRefusal(const std::filesystem::path& targetPath,
                                                      DegradedSavePolicy policy) const
{
    // Play mode is asked first and answers absolutely. SaveAnyway is the user confirming a
    // DEGRADED overwrite; it is not, and must never become, a way to write a simulated world
    // over the authored one — that write has no confirmed form.
    if (IsSaveBlockedByPlayMode())
        return kSaveBlockedDuringPlayMessage;
    if (WouldRefuseDegradedOverwrite(targetPath, policy))
        return DescribeDegradedRefusal(targetPath);
    return {};
}

// The single refusal both write paths take, so a site cannot acquire one guard and miss the other.
bool SceneDocumentManager::RefuseSaveWrite(const std::filesystem::path& targetPath,
                                           DegradedSavePolicy policy) const
{
    const std::string reason = DescribeSaveRefusal(targetPath, policy);
    if (reason.empty())
        return false;
    Logger::Log::Error("SceneDocument: {}", reason);
    return true;
}

bool SceneDocumentManager::Save(DegradedSavePolicy degradedPolicy)
{
    if (!m_World)
        return false;
    if (!m_ScenePath.has_value())
        return false;
    // Before ANY sidecar is flushed: the zone/sculpt/material-library writes below are themselves
    // destructive, so a refusal that happened after them would still have modified the project.
    if (RefuseSaveWrite(*m_ScenePath, degradedPolicy))
        return false;

    Scene::SaveOptions opts{};
    auto& am = EngineCore::GetInstance().GetAssetManager();
    auto& reg = am.GetRegistry();
    EditorSceneAssetResolver resolver(am, EngineCore::GetInstance().GetResolvedAssetRoot());
    opts.assetResolver = &resolver;
    opts.assetRootOverride = resolver.GetAssetRoot();
    Scene::SceneHierarchyUi hierarchySnap{};
    const Scene::SceneHierarchyUi* hierarchyPtr = nullptr;
    if (m_HierarchyUiCaptureForSave)
    {
        hierarchySnap = m_HierarchyUiCaptureForSave();
        hierarchyPtr = &hierarchySnap;
    }
    opts.hierarchyUi = hierarchyPtr;
    Scene::SceneEditorCamera cameraSnap{};
    const Scene::SceneEditorCamera* cameraPtr = nullptr;
    if (m_EditorCameraCaptureForSave)
    {
        cameraSnap = m_EditorCameraCaptureForSave();
        cameraPtr = &cameraSnap;
    }
    opts.editorCamera = cameraPtr;
    // Persist brush-authored terrain zone payloads and the planet's sphere sculpt
    // (+ swap their refs to the file-backed GUIDs) before the scene captures them.
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
    {
        Editor::FlushDirtyZonePayloads(*m_World, *m_ScenePath, reg, *terrain,
                                       Editor::ZonePayloadFlushMode::SaveMintAndSwap,
                                       &m_ZoneStagingLedger);
        Editor::FlushDirtySphereSculpt(*m_World, *m_ScenePath, reg, *terrain,
                                       Editor::ZonePayloadFlushMode::SaveMintAndSwap,
                                       &m_ZoneStagingLedger);
    }
    // A terrain authored before material libraries existed gains one here, minted from the
    // per-layer fields it already shades with, so the scene captures the reference below. Same
    // mint-and-swap contract as the payloads above; a terrain that already binds one is untouched.
    Editor::MintMissingTerrainMaterialLibraries(*m_World, *m_ScenePath,
                                                EngineCore::GetInstance().GetAssetManager());
    // Announced before the file is touched: a file watcher can report the save landing
    // before this thread gets to, and the announcement is what marks that report as this
    // write arriving rather than as a second change.
    auto sceneWrite = EngineCore::GetInstance().GetAssetManager().ExpectWrite(*m_ScenePath);
    const bool ok = Scene::SaveSceneToFile(*m_World, *m_ScenePath, opts);
    if (!ok)
        return false;

    // The user accepted this overwrite, and the file on disk is now a faithful copy of the world.
    // Whatever could not be read is either back in the file verbatim or gone by consent — either
    // way there is nothing left for the guard to protect, so it must stop asking.
    m_LastLoadDegraded.reset();
    RefreshDisplayNameForDegradation();

    MarkClean();

    // Report our own write: this is the change drive, so the asset's registry metadata
    // never lags the file just produced, on any host.
    sceneWrite.Report(*m_ScenePath);
    Editor::RequestTerrainBakeStoreForSave(*m_World, m_ScenePath, *m_ScenePath, reg);

    // Wake the VCS poll so the panel reflects the saved scene on the next
    // tick instead of waiting up to one polling interval.
    if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
        vcs->RefreshStatus();

    return true;
}

bool SceneDocumentManager::SaveAs(const std::filesystem::path& scenePath, DegradedSavePolicy degradedPolicy)
{
    if (!m_World)
        return false;
    // Save As to a DIFFERENT path is the sanctioned escape for a degraded document: the source file
    // survives untouched, so there is nothing to guard. Targeting the document's own path is a Save
    // wearing a different name — the debug server can ask for exactly that — and takes the guard.
    //
    // The play-mode half of this refusal has no such escape, and DescribeSaveRefusal applies it to
    // every target: Save As adopts the new path as the document identity, so a play-time snapshot
    // would leave the document naming the snapshot once play restores the authored world, and the
    // terrain mint-and-swap below would rewrite zone payload refs that play mode holds a pre-play
    // snapshot of.
    if (RefuseSaveWrite(scenePath, degradedPolicy))
        return false;

    // Announced above every touch of the target, not just the last one: Save As lands on
    // the path twice — the preservation copy below, then the save itself — and a watcher
    // reports the first one the moment it appears. Reporting once at the end is what makes
    // the pair a single change.
    auto sceneWrite = EngineCore::GetInstance().GetAssetManager().ExpectWrite(scenePath);

    // Best-effort: if we are saving-as from an existing scene file, copy it first so
    // comment/include/resource/embed preservation can carry over into the new file.
    if (m_ScenePath.has_value())
    {
        std::error_code ec;
        std::filesystem::create_directories(scenePath.parent_path(), ec);
        ec.clear();
        std::filesystem::copy_file(*m_ScenePath, scenePath, std::filesystem::copy_options::overwrite_existing, ec);
    }

    Scene::SaveOptions opts{};
    auto& am = EngineCore::GetInstance().GetAssetManager();
    auto& reg = am.GetRegistry();
    EditorSceneAssetResolver resolver(am, EngineCore::GetInstance().GetResolvedAssetRoot());
    opts.assetResolver = &resolver;
    opts.assetRootOverride = resolver.GetAssetRoot();
    Scene::SceneHierarchyUi hierarchySnap{};
    const Scene::SceneHierarchyUi* hierarchyPtr = nullptr;
    if (m_HierarchyUiCaptureForSave)
    {
        hierarchySnap = m_HierarchyUiCaptureForSave();
        hierarchyPtr = &hierarchySnap;
    }
    opts.hierarchyUi = hierarchyPtr;
    Scene::SceneEditorCamera cameraSnap{};
    const Scene::SceneEditorCamera* cameraPtr = nullptr;
    if (m_EditorCameraCaptureForSave)
    {
        cameraSnap = m_EditorCameraCaptureForSave();
        cameraPtr = &cameraSnap;
    }
    opts.editorCamera = cameraPtr;
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
    {
        Editor::FlushDirtyZonePayloads(*m_World, scenePath, reg, *terrain,
                                       Editor::ZonePayloadFlushMode::SaveMintAndSwap,
                                       &m_ZoneStagingLedger);
        Editor::FlushDirtySphereSculpt(*m_World, scenePath, reg, *terrain,
                                       Editor::ZonePayloadFlushMode::SaveMintAndSwap,
                                       &m_ZoneStagingLedger);
    }
    Editor::MintMissingTerrainMaterialLibraries(*m_World, scenePath,
                                                EngineCore::GetInstance().GetAssetManager());
    const bool ok = Scene::SaveSceneToFile(*m_World, scenePath, opts);
    if (!ok)
        return false;

    const std::optional<std::filesystem::path> previousScenePath = m_ScenePath;
    m_ScenePath = scenePath;
    m_DisplaySceneName = scenePath.filename().string();
    // Save As is how a user adopts a half-loaded world deliberately, under a name
    // they chose. Once it is on disk it is a document again, not a failed load.
    m_LastLoadFailure.reset();
    m_PartialLoadDocument.reset();
    // Whatever this world is, the file now on disk is a faithful copy of it, so the document and its
    // source agree again and there is nothing left for the guard to protect.
    m_LastLoadDegraded.reset();
    MarkClean();

    // Report our own write: without it the scene just authored has no registry identity
    // until something else scans for it.
    sceneWrite.Report(scenePath);
    Editor::RequestTerrainBakeStoreForSave(*m_World, previousScenePath, scenePath, reg);

    // Wake the VCS poll: a new file appeared (or the target was overwritten)
    // and the panel should reflect it on the next tick.
    if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
        vcs->RefreshStatus();

    return true;
}

bool SceneDocumentManager::SaveBackupCopy()
{
    if (!m_World || !m_ScenePath.has_value())
        return false;

    // Autosave is unattended, so it can never ask. It still backs up a degraded scene whose text was
    // fully preserved — that backup round-trips and crash protection is worth keeping — but it will
    // not write one that DROPS data, because crash recovery would later restore that stripped file
    // as though it were the user's work.
    if (m_LastLoadDegraded.has_value() && m_LastLoadDegraded->DroppedCount() > 0)
        return false;

    auto& am = EngineCore::GetInstance().GetAssetManager();
    auto& reg = am.GetRegistry();

    // Stage dirty zone payloads BACKUP-SIDE (`<live>.tzone.backup` siblings).
    // The live .tzone files are written only by explicit Save: close-without-
    // saving must genuinely discard strokes, so the timer's bytes sit in
    // staging until the crash-recovery prompt promotes them (accept) or
    // deletes them (decline / clean close). Idempotent per stroke via the
    // ledger — an idle interval re-stages nothing.
    uint32_t stagedPayloads = 0;
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
    {
        stagedPayloads = Editor::FlushDirtyZonePayloads(
            *m_World, *m_ScenePath, reg, *terrain,
            Editor::ZonePayloadFlushMode::BackupExistingOnly, &m_ZoneStagingLedger);
        stagedPayloads += Editor::FlushDirtySphereSculpt(
            *m_World, *m_ScenePath, reg, *terrain,
            Editor::ZonePayloadFlushMode::BackupExistingOnly, &m_ZoneStagingLedger);
    }

    // Only write the backup scene when there is unsaved work to protect. An
    // unconditional rewrite would leave a "newer than the scene" backup after
    // every idle interval, turning a crash in a clean session into a spurious
    // recovery prompt on the next open.
    if (!m_Dirty && stagedPayloads == 0)
        return false;

    const std::filesystem::path& src = *m_ScenePath;
    const std::filesystem::path backupPath = SceneBackupPathFor(src);

    std::error_code ec;
    std::filesystem::create_directories(backupPath.parent_path(), ec);
    ec.clear();
    std::filesystem::copy_file(src, backupPath, std::filesystem::copy_options::overwrite_existing, ec);

    Scene::SaveOptions opts{};
    EditorSceneAssetResolver resolver(am, EngineCore::GetInstance().GetResolvedAssetRoot());
    opts.assetResolver = &resolver;
    opts.assetRootOverride = resolver.GetAssetRoot();
    Scene::SceneHierarchyUi hierarchySnap{};
    const Scene::SceneHierarchyUi* hierarchyPtr = nullptr;
    if (m_HierarchyUiCaptureForSave)
    {
        hierarchySnap = m_HierarchyUiCaptureForSave();
        hierarchyPtr = &hierarchySnap;
    }
    opts.hierarchyUi = hierarchyPtr;
    Scene::SceneEditorCamera cameraSnap{};
    const Scene::SceneEditorCamera* cameraPtr = nullptr;
    if (m_EditorCameraCaptureForSave)
    {
        cameraSnap = m_EditorCameraCaptureForSave();
        cameraPtr = &cameraSnap;
    }
    opts.editorCamera = cameraPtr;
    return Scene::SaveSceneToFile(*m_World, backupPath, opts);
}

std::vector<uint8_t> SceneDocumentManager::CaptureWorldSnapshot() const
{
    if (!m_World)
        return {};
    return m_World->SerializeWorld();
}

bool SceneDocumentManager::ApplyWorldSnapshot(const std::vector<uint8_t>& snapshot) const
{
    if (!m_World)
        return false;
    m_World->DeserializeWorld(snapshot);
    m_World->ProcessCommands();
    return true;
}

void SceneDocumentManager::Unsubscribe()
{
    if (m_Notifications)
    {
        if (m_SubComponent)
            m_Notifications->Unsubscribe(m_SubComponent);
        if (m_SubStructure)
            m_Notifications->Unsubscribe(m_SubStructure);
    }
    m_SubComponent = {};
    m_SubStructure = {};
}

void SceneDocumentManager::Subscribe()
{
    if (!m_Notifications)
        return;

    m_SubComponent = m_Notifications->SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (!m_World || e.world != m_World)
                return;
            if (e.kind == EditorChangeNotifications::ChangeKind::Preview ||
                e.kind == EditorChangeNotifications::ChangeKind::InspectorRebuild)
                return;
            MarkDirty();
        });

    m_SubStructure = m_Notifications->SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& e)
        {
            if (!m_World || e.world != m_World)
                return;
            if (e.kind != EditorChangeNotifications::ChangeKind::Commit)
                return;
            MarkDirty();
        });
}

} // namespace GameEngine::Editor

#pragma once

#include "EditorChangeNotifications.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneLoadFailure.h"
#include "Terrain/TerrainZonePayloadSaver.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Engine::Renderer
{
class RenderServices;
}

namespace Editor
{

// Refusal text shared by every manual-save entry point that declines while play
// mode is active. It has to read correctly in all four non-Edit states, so it
// names the destination (Edit) as the requirement and makes the stop CONDITIONAL:
// ChangeReview is reached after play has already stopped, where an unconditional
// "stop play" is an instruction the user cannot carry out. The closing clause
// names the flow that does carry play-time editor edits into the authored scene —
// PlayModeManager stages the undo commands recorded during play, and the change
// review applies or discards them.
inline constexpr const char* kSaveBlockedDuringPlayMessage =
    "Scene save is disabled until play mode returns to Edit - stop play if it is running, "
    "then apply or discard the play-time edits the change review offers.";

// Editor-side state for the currently edited world as a .scene document.
// This tracks:
// - which .scene file (if any) backs the current world
// - whether the world has unsaved changes
// - a baseline ECS snapshot at last save/load (used for fast dirty checking if needed)
class SceneDocumentManager
{
  public:
    SceneDocumentManager() = default;
    ~SceneDocumentManager();

    void SetWorld(ECS::World* world);
    void SetChangeNotifications(EditorChangeNotifications* notifications);

    ECS::World* GetWorld() const { return m_World; }

    bool HasScenePath() const { return m_ScenePath.has_value(); }
    const std::optional<std::filesystem::path>& GetScenePath() const { return m_ScenePath; }
    std::optional<std::string> GetDisplaySceneName() const;

    // The last scene open that failed. A failure whose WorldCleared is set means
    // the document has NO path: the world holds a partial load, so this manager
    // refuses to keep naming (and therefore refuses to let Save overwrite) the
    // scene that is no longer in it.
    //
    // Cleared by whatever gives the world a document again — a successful
    // replace or recovery open, NewSceneUntitled, or SaveAs. A successful
    // ADDITIVE load clears it only when the standing failure left the document
    // intact: additive restores nothing and names nothing, so on a cleared world
    // it just adds entities to the partial load, and dropping the record there
    // would resume claiming the world is a scene.
    const std::optional<SceneLoadFailure>& GetLastLoadFailure() const { return m_LastLoadFailure; }

    // The document a failed open left partly loaded in the world, for as long as the world holds
    // that partial load. It outlives the failure record that set it: a later open that fails
    // before the clear replaces the record but leaves this world in place. Cleared by what gives
    // the world a document again (a successful replace or recovery open, NewSceneUntitled, SaveAs);
    // an additive load adds to the partial load and leaves it standing.
    const std::optional<std::filesystem::path>& GetPartialLoadDocument() const { return m_PartialLoadDocument; }

    // The last scene open that loaded but could not apply everything in the file. Unlike a failure,
    // the document is real and keeps its path — this is what Save interposes on, and what tells the
    // user which assignments are missing before they overwrite the file.
    //
    // Cleared by whatever gives the world a different document (a successful replace/recovery open,
    // NewSceneUntitled, SaveAs to a new path) and by an accepted SaveAnyway, after which the file on
    // disk and the world agree again.
    // The record EXACTLY AS THE LOAD LEFT IT. Correct for anything asking what that load did, which
    // is why the title decoration and the autosave gate read it. Anything reporting what a SAVE
    // will now do wants the outstanding view below instead: an override can retire after the load,
    // and this record cannot see it happen.
    const std::optional<SceneLoadDegraded>& GetLastLoadDegraded() const { return m_LastLoadDegraded; }

    // The same record filtered to the skips still outstanding against the world — see
    // Scene::SkipIsOutstanding. This is what the debug surface reports and what the save guard and
    // its prompt count, so that a field the user discarded stops being described as text a save
    // will write back. Allocates; take it once per report rather than per row.
    std::optional<SceneLoadDegraded> GetOutstandingLoadDegraded() const;

    // Cheap count of the above, for the guard's yes/no. Zero means every unreadable assignment has
    // been adjudicated and there is nothing left for the guard to protect.
    std::size_t OutstandingDegradedSkipCount() const;

    // Whether a save aimed at `targetPath` would be refused because this document did not fully
    // load and that path is the file it was read from. Callers use it to tell that refusal apart
    // from an ordinary write failure, which has no force/confirm escape to point the user at.
    bool WouldRefuseDegradedOverwrite(const std::filesystem::path& targetPath,
                                      DegradedSavePolicy policy) const;

    bool IsDirty() const { return m_Dirty; }
    void MarkDirty();
    void MarkClean();

    // Replace the world contents with the given scene.
    bool OpenSceneReplace(const std::filesystem::path& scenePath);
    // Crash-recovery accept: load the backup scene's CONTENT but keep `scenePath`
    // as the document identity, and open dirty — the recovered state is unsaved
    // until the user explicitly saves it.
    bool OpenSceneRecovered(const std::filesystem::path& backupScenePath,
                            const std::filesystem::path& scenePath);
    // Add entities from a scene into the current world.
    bool LoadSceneAdditive(const std::filesystem::path& scenePath);
    // Clears the world and starts a new untitled scene with default entities
    // (directional light, ambient light, sky environment, plane, sphere).
    void NewSceneUntitled(Engine::Renderer::RenderServices* renderServices = nullptr);

    // Play mode simulates the authored world IN PLACE (PlayModeManager keeps a
    // same-world snapshot and restores it on exit), so while it runs the world
    // holds runtime state, not authored state. The probe reports "play mode is
    // not in its Edit state". Unset means never blocked — a headless or
    // standalone document has no play mode.
    void SetPlayModeProbe(std::function<bool()> isNotEditing);
    bool IsSaveBlockedByPlayMode() const;

    // Why a write to `targetPath` would be refused, or empty when it may proceed.
    // Play mode is asked FIRST and is NOT overridable: DegradedSavePolicy::SaveAnyway
    // confirms a degraded overwrite, never a write of a simulated world.
    std::string DescribeSaveRefusal(const std::filesystem::path& targetPath,
                                    DegradedSavePolicy policy) const;

    // Save to current scene path (returns false if untitled).
    //
    // Refuses while play mode is active (see DescribeSaveRefusal), and refuses a document that
    // loaded DEGRADED unless the caller passes SaveAnyway: writing back a
    // scene this build could not fully read replaces the user's unreadable data with whatever the
    // fields defaulted to, and the source file is the one copy of it that exists. The refusal
    // happens before any sidecar (.tzone/.tsculpt/material-library) is touched, so a refused save
    // writes nothing at all.
    bool Save(DegradedSavePolicy degradedPolicy = DegradedSavePolicy::Refuse);
    // Save to a new path and set it as the document path. Saving a degraded document to a DIFFERENT
    // path is the sanctioned escape (the source survives untouched); targeting the document's own
    // path is a Save in disguise and takes the same guard. Play mode refuses either way — a
    // runtime snapshot under a new name would still steal the document identity.
    bool SaveAs(const std::filesystem::path& scenePath,
                DegradedSavePolicy degradedPolicy = DegradedSavePolicy::Refuse);

    // Autosave tick: stage dirty terrain zone payloads to `.tzone.backup` siblings
    // and, when there is unsaved work, save the world to `<stem>.backup<ext>`
    // beside the active scene file. Never touches the live scene or .tzone files
    // (explicit Save owns those) and does not change the document path or
    // dirty/clean state. Returns true when anything was written; false when there
    // is nothing to back up, no active scene path, or the save fails.
    bool SaveBackupCopy();

    // Editor hierarchy foldout/selection snapshot for the next save (embedded as [hierarchy_ui]).
    void SetHierarchyUiCaptureForSave(std::function<Scene::SceneHierarchyUi()> capture);

    // Consumes embedded [hierarchy_ui] state from the last OpenSceneReplace / NewSceneUntitled (once).
    std::optional<Scene::SceneHierarchyUiFromFile> ConsumeHierarchyUiFromLastDocumentLoad();

    // Editor scene-view camera pose snapshot for the next save (embedded as [editor_camera]).
    void SetEditorCameraCaptureForSave(std::function<Scene::SceneEditorCamera()> capture);

    // Consumes embedded [editor_camera] state from the last OpenSceneReplace (once).
    std::optional<Scene::SceneEditorCameraFromFile> ConsumeEditorCameraFromLastDocumentLoad();

    // Releases the editor-side spline generators (placement / fence / extrude),
    // which cache EntityHandles into the open scene's world and destroy them on
    // a later sweep. Runs inside ReleaseSceneScopedState, against the still-live
    // outgoing world. EditorApplication owns those generators, so it supplies
    // the call; without it their handles survive the swap and World::Clear's
    // version restart makes them alias — and destroy — entities of the incoming
    // scene (SceneSwapOrderingTests).
    void SetSceneScopedGeneratorRelease(std::function<void()> release);

    // Internal ECS snapshot support (Undo/Redo, play mode clone/restore).
    std::vector<uint8_t> CaptureWorldSnapshot() const;
    bool ApplyWorldSnapshot(const std::vector<uint8_t>& snapshot) const;

    // Q6 slice 3b: an in-place device rebuild frees the baked HLOD proxy submeshes
    // this scene registered (runtime-generated, so slice 4's asset re-upload does
    // not cover them). Poll the device rebuild generation and, when it moves,
    // re-reconcile — HlodRuntime::ReconcileScene Retires the dead proxies and
    // respawns + re-registers them from the .gehlod cache. Call once per frame from
    // the scene controller; no-op when no rebuild happened or no scene is loaded.
    void PollDeviceRebuildAndRecoverHlod();

    // Drive the deferred scene-build resolve for one frame. A replace-open loads
    // the scene file synchronously but defers the per-entity GPU mesh/material
    // resolve to this budgeted pump so the editor keeps presenting frames while
    // the scene materializes. No-op when no build is in progress. When the pump
    // drains, runs the one-shot tail (HLOD reconcile, structure notify, MarkClean).
    void PumpSceneBuild(std::chrono::milliseconds budget);

    // Resolve any in-flight scene build to completion right now (runs the tail).
    // Called before a world-mutating edit / undo capture / undo-redo so those
    // never touch a half-resolved world — an unresolved MeshRenderer baked into
    // an undo snapshot re-hides the entity permanently. No-op when no build is
    // active, and (like the pump) a no-op while the device is not Healthy.
    void DrainSceneBuild();

    // True while a deferred scene build is resolving (open began, pump not yet
    // drained). Consumers gate interactions that a half-built world would break
    // (play-mode enter, autosave, undoable edits).
    bool IsSceneBuildInProgress() const { return m_BuildActive; }

    // Progress for a scene-view load indicator. Returns false (leaving args
    // untouched) when no build is active.
    bool GetSceneBuildProgress(uint64_t& outProcessed, uint64_t& outTotal) const;

  private:
    void Unsubscribe();
    void Subscribe();

    // Shared open path: load `contentPath` into the world, adopt `documentPath`
    // as the document identity (recovery loads backup content under the real
    // scene's identity). `openAsDirty` opens the document dirty once the build
    // finishes (crash-recovery: the recovered state is unsaved).
    bool OpenSceneFromFile(const std::filesystem::path& contentPath,
                           const std::filesystem::path& documentPath,
                           bool openAsDirty);

    // Run the one-shot tail of a scene open once the resolve pump drains: HLOD
    // reconcile (skipped when `renderServices` is null — headless), structure
    // notify, and MarkClean (+ MarkDirty for a recovery open). Clears build state.
    void FinishSceneBuild(Engine::Renderer::RenderServices* renderServices);

    // Drop an in-flight build without finishing it. A replace-open cancels the
    // previous scene's build before World::Clear invalidates its pending handles.
    void CancelSceneBuild();

    // Release everything scoped to the OUTGOING scene, immediately before the
    // world is swapped: the in-flight resolve build, this scene's HLOD proxy
    // clusters, and the resident terrain zone-payload / planet-sculpt edits.
    //
    // Must run BEFORE World::Clear, not after. Clear recycles entity indices and
    // restarts versions in place, so a cluster's proxy handle held across it
    // aliases an unrelated freshly-loaded entity that IsValid cannot distinguish
    // — and Retire would destroy it.
    //
    // Callers are the two swap sites, OpenSceneFromFile and NewSceneUntitled.
    // NEVER LoadSceneAdditive: that path keeps the world, so retiring proxies
    // there destroys the surviving scene's far-field geometry and the payload
    // reset discards its live terrain edits.
    void ReleaseSceneScopedState(Engine::Renderer::RenderServices* renderServices);

    // Reconcile (or retire) this scene's baked HLOD proxy clusters against the
    // current world. Shared by OpenSceneReplace and the device-rebuild recovery.
    void ReconcileHlodForScene(const std::filesystem::path& scenePath,
                               Engine::Renderer::RenderServices& renderServices);

    ECS::World* m_World = nullptr; // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned

    std::function<bool()> m_PlayModeProbe;

    EditorChangeNotifications::SubscriptionToken m_SubComponent{};
    EditorChangeNotifications::SubscriptionToken m_SubStructure{};

    // Announce and record the failure Scene::GetLastSceneIOError() currently
    // describes, and — when it says the world was already cleared — drop the
    // document identity, so nothing downstream reports a scene that is not in
    // the world. `contentPath` is the file the load actually read (a recovery
    // open reads a backup), used only when the error names no file itself.
    void RecordLoadFailure(const std::filesystem::path& documentPath,
                           const std::filesystem::path& contentPath);

    // Announce and record `census` against `documentPath` when it holds anything, or clear a
    // standing record when it does not. Called after every successful document load.
    void RecordLoadDegradation(const std::filesystem::path& documentPath,
                               Scene::SceneLoadDegradation census);

    // The degraded-overwrite refusal text for `targetPath`, naming what a save would cost.
    // Only valid while a degraded record stands (WouldRefuseDegradedOverwrite gates the call).
    std::string DescribeDegradedRefusal(const std::filesystem::path& targetPath) const;

    // The one refusal both write paths take: logs whichever reason applies and returns true.
    bool RefuseSaveWrite(const std::filesystem::path& targetPath, DegradedSavePolicy policy) const;

    // Re-derives the display name from the scene path, decorating it while a degraded record stands.
    void RefreshDisplayNameForDegradation();

    std::optional<std::filesystem::path> m_ScenePath;
    std::optional<std::string> m_DisplaySceneName;
    std::optional<SceneLoadFailure> m_LastLoadFailure;
    std::optional<std::filesystem::path> m_PartialLoadDocument;
    std::optional<SceneLoadDegraded> m_LastLoadDegraded;
    bool m_Dirty = false;
    std::vector<uint8_t> m_BaselineSnapshot;

    std::optional<Scene::SceneHierarchyUiFromFile> m_PendingHierarchyUiFromLoad;
    std::function<Scene::SceneHierarchyUi()> m_HierarchyUiCaptureForSave;

    std::optional<Scene::SceneEditorCameraFromFile> m_PendingEditorCameraFromLoad;
    std::function<Scene::SceneEditorCamera()> m_EditorCameraCaptureForSave;

    std::function<void()> m_SceneScopedGeneratorRelease;

    // Last device rebuild generation seen by PollDeviceRebuildAndRecoverHlod, so
    // the HLOD re-reconcile runs at most once per rebuild.
    uint64_t m_LastDeviceRebuildGeneration = 0;

    // Deferred scene-build state. A replace-open Begins the pump and returns; the
    // per-frame PumpSceneBuild drains it, then FinishSceneBuild runs the tail.
    Engine::Renderer::SceneBuildPump m_BuildPump;
    std::optional<std::filesystem::path> m_BuildDocumentPath;
    bool m_BuildActive = false;
    bool m_BuildOpenAsDirty = false;
    std::chrono::steady_clock::time_point m_BuildStartTime{};

    // Main-thread CPU actually spent inside the resolve pump, summed over every
    // step. Distinct from the wall-clock the build spans: the pump is budgeted
    // per frame, so wall-clock counts the frames it waited through and says
    // nothing about how much work the resolve costs. Only the CPU figure can
    // tell "the pump is expensive" from "the pump is spread thin".
    double m_BuildPumpCpuMs = 0.0;

    // Autosave payload staging bookkeeping (reset on document load): which
    // DataVersion each zone was last staged at, so an idle timer tick re-stages
    // nothing. See ZonePayloadFlushMode::BackupExistingOnly.
    ZonePayloadStagingLedger m_ZoneStagingLedger;
};

} // namespace Editor
} // namespace GameEngine

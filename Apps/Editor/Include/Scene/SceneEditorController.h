#pragma once

#include "Scene/SceneIO.h"
#include "Scene/SceneLoadFailure.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class ConfirmActionModal;
class RestoreSceneBackupModal;
class SaveSceneAsModal;
class SaveSceneChangesModal;
class UIElement;
namespace ECS
{
class World;
}
namespace Input
{
class InputSystem;
}

// Editor glue for scene authoring UX:
// - Handles Ctrl+S (save/save-as)
// - Handles open scene (replace/additive) with dirty prompting
// - Owns SceneDocumentManager + scene modals
namespace Editor
{
class UndoRedoService;
class EditorChangeNotifications;
class SceneDocumentManager;

class SceneEditorController
{
  public:
    SceneEditorController();
    ~SceneEditorController();

    void AttachToRoot(UIElement& root,
                      ECS::World& world,
                      Editor::EditorChangeNotifications& notifications,
                      Editor::UndoRedoService* undoRedo);

    // Called per frame from EditorApplication::Update (outside UI callbacks).
    void Update(Input::InputSystem* inputSystem);

    // Hook from AssetsPanel double-click open.
    // Returns true if the controller handled the open.
    bool HandleAssetOpen(const std::filesystem::path& path);

    // Headless/automation answer for the prompts an open can raise. Prompt is
    // the interactive default. Discard and Restore both resolve the dirty-scene
    // prompt as Don't Save; they differ on the crash-recovery prompt: Discard
    // deletes the autosave staging and opens the last saved state, Restore
    // promotes it and opens the autosaved work.
    enum class OpenRestorePolicy
    {
        Prompt,
        Discard,
        Restore
    };

    // Hook from Assets context menu scene actions.
    void RequestOpenScene(const std::filesystem::path& path, bool additive,
                          OpenRestorePolicy restorePolicy = OpenRestorePolicy::Prompt);

    // Create a fresh untitled scene (prompts to save if dirty).
    void RequestNewScene();

    // Top toolbar save icon / menu: same behavior as Editor.Global.SaveScene (Ctrl+S).
    void RequestSaveScene();
    /// Save As dialog (e.g. toolbar save with Shift held).
    void RequestSaveSceneAs();
    // Discard unsaved edits. Saved documents reload from disk; untitled
    // documents become a new empty scene. Shows a confirmation first.
    // Play mode is a no-op (the simulated world must not replace the document).
    void RequestRevertScene();

    // Play-state probe forwarded to the document manager, which refuses saves for
    // as long as it reports true. See SceneDocumentManager::SetPlayModeProbe.
    // Held until AttachToRoot builds the document if it arrives before that.
    void SetPlayModeProbe(std::function<bool()> isNotEditing);

    // Synchronous save for automation/IPC (no modal). Both route through
    // SceneDocumentManager::Save / SaveAs so hierarchy-UI capture + MarkClean +
    // recent-list + OnAfterSave happen exactly as the menu path does.
    // Both refuse while play mode runs, reporting kSaveBlockedDuringPlayMessage.
    // Save to the active scene path; fails (sets *outError) when the document is untitled.
    // Automation save. Refuses a degraded document unless `degradedPolicy` says otherwise, and
    // reports why in outError so the caller can print the loss before deciding.
    bool SaveActiveScene(std::string* outError,
                         DegradedSavePolicy degradedPolicy = DegradedSavePolicy::Refuse);
    // Save to `scenePath` and adopt it as the active document path.
    bool SaveActiveSceneAs(const std::filesystem::path& scenePath, std::string* outError,
                           DegradedSavePolicy degradedPolicy = DegradedSavePolicy::Refuse);

    // Periodic auto-save to `<name>.backup.scene` (see settings). Call from EditorApplication::Update.
    void TickAutoSave(float deltaSeconds, bool inhibitWhileNotEditing);

    /// If the active scene document is dirty, shows SaveSceneChangesModal; otherwise invokes `proceedQuit` immediately.
    /// When the user Saves (or chooses Don't Save / completes Save As), `proceedQuit` runs exactly once.
    void PromptSaveBeforeQuit(std::function<void()> proceedQuit);

    // Clean close of the active document (normal exit, scene switch): delete its
    // autosave staging — the `.backup.scene` and any staged `.tzone.backup`
    // siblings — so the next open of this scene sees no recovery prompt. Only a
    // crash skips this, which is exactly what recovery staging is for. Called on
    // scene switch and from the destructor; no-op for untitled documents.
    void CleanupBackupsForCleanClose();

    // Snapshot helpers (used by undo/redo and Play Mode).
    std::vector<std::uint8_t> CaptureWorldSnapshot() const;
    bool ApplyWorldSnapshot(const std::vector<std::uint8_t>& snapshot) const;

    std::optional<std::filesystem::path> GetActiveScenePath() const;
    std::optional<std::string> GetActiveSceneDisplayName() const;
    std::vector<std::filesystem::path> GetRecentScenePaths() const;

    // The last scene open that failed. A replace-open failure that had already
    // cleared the world leaves NO active scene path — the entities on screen are
    // a partial load, and this is the only thing that says so.
    const std::optional<SceneLoadFailure>& GetLastSceneLoadFailure() const;

    // The last scene open that loaded without being able to apply everything in the file, exactly
    // as that load left it: see SceneDocumentManager::GetLastLoadDegraded.
    const std::optional<SceneLoadDegraded>& GetLastSceneLoadDegraded() const;

    // The same record filtered to what is still outstanding, which is what Save is guarded on and
    // what any surface reporting on a save must use: see
    // SceneDocumentManager::GetOutstandingLoadDegraded.
    std::optional<SceneLoadDegraded> GetOutstandingSceneLoadDegraded() const;

    bool IsSceneDirty() const;
    void MarkSceneDirty();

    // True while a replace-open's deferred entity resolve is still draining across
    // frames (see SceneDocumentManager::PumpSceneBuild). Gates interactions that a
    // half-built world would corrupt — play-mode enter, autosave, undoable edits.
    bool IsSceneBuildInProgress() const;
    // Resolve any in-flight scene build to completion now. Wired to the undo
    // service's before-mutation hook so any edit / undo / redo operates on a fully
    // resolved world instead of baking unresolved entities into history.
    void DrainSceneBuild();
    // Scene-build progress for a load indicator; false when no build is active.
    bool GetSceneBuildProgress(uint64_t& outProcessed, uint64_t& outTotal) const;

    bool IsAnyModalOpen() const;
    // "" when no scene modal is visible, else saveChanges | saveAs | restoreBackup | revertScene |
    // loadFailed.
    std::string GetActiveModalKind() const;
    // Automation: answer the visible scene modal by choice name —
    // saveChanges: save | dontSave | cancel; restoreBackup: restore | discard;
    // saveAs: cancel only (Save needs the name field + overwrite flow);
    // revertScene: confirm | cancel; loadFailed: ok.
    // Returns false with *outError when no modal is visible or the choice
    // doesn't apply to it.
    bool RespondToActiveModal(const std::string& choice, std::string* outError);

    void SetHierarchyUiCaptureForSave(std::function<Scene::SceneHierarchyUi()> capture);
    std::optional<Scene::SceneHierarchyUiFromFile> ConsumeHierarchyUiFromLastDocumentLoad();

    void SetEditorCameraCaptureForSave(std::function<Scene::SceneEditorCamera()> capture);
    std::optional<Scene::SceneEditorCameraFromFile> ConsumeEditorCameraFromLastDocumentLoad();

    // Releases the editor-side spline generators against the outgoing world on
    // every scene swap (SceneDocumentManager::SetSceneScopedGeneratorRelease).
    void SetSceneScopedGeneratorRelease(std::function<void()> release);

    // Callback invoked after a successful scene save (Save or Save-As).
    // Receives the absolute path of the saved scene file.
    void SetOnAfterSave(std::function<void(const std::filesystem::path&)> callback)
    {
        m_OnAfterSave = std::move(callback);
    }

    // Eager scene persistence (ApplicationConfig::PersistScenesEagerly): name a
    // new scene on creation and prompt once to save an untitled one, so auto-save
    // (which needs a file) can run. Off, the controller behaves as before.
    void SetEagerScenePersistence(bool enabled) { m_EagerScenePersistence = enabled; }
    // Reports true while boot still owns the scene: the project picker is open
    // (its modal owns the screen) or a startup scene is queued but not yet
    // requested. The untitled-scene save prompt defers to it, so it never asks
    // about the interim empty scene a startup scene is about to replace.
    void SetBootFlowActiveProbe(std::function<bool()> probe)
    {
        m_BootFlowActiveProbe = std::move(probe);
    }

  private:
    struct PendingAction
    {
        enum class Kind
        {
            None,
            NewScene,
            OpenReplace,
            OpenAdditive,
            // Crash-recovery accept: open the backup scene's content under the
            // real scene's document identity (path = real scene; the backup path
            // rides in m_RecoveredBackupScenePath).
            OpenRecovered
        };
        Kind kind = Kind::None;
        std::filesystem::path path;
        OpenRestorePolicy restorePolicy = OpenRestorePolicy::Prompt;
    };

    void Execute(const PendingAction& a);
    // A replace-open that failed AFTER World::Clear leaves an undo stack whose
    // entries name a world that no longer exists. Called on the failure branch
    // of both replace-open paths; no-op for a failure that never reached the
    // clear, whose document — and therefore whose history — is intact.
    void DropUndoHistoryIfTheWorldWasCleared();
    // Which open failed: the record says whether the world was cleared, but not
    // whether the open was additive or a crash recovery, nor whether an additive
    // open left entities behind.
    enum class LoadFailureKind
    {
        Replace,
        Recovered,
        Additive,        // nothing of the file reached the world
        AdditivePartial  // entities created before the error stay in the world
    };
    // What the world held just before an open. A failed open reports against
    // this, and it must be taken before the open: a failure after the clear
    // drops the document path, and with it the name of what was there.
    struct OutgoingWorld
    {
        std::string SceneName;     // file name of the open document; empty when it has none
        std::string PartialLoadOf; // file a previous failed open left partly loaded (SceneDocumentManager::
                                   // GetPartialLoadDocument); empty otherwise
    };
    OutgoingWorld CaptureOutgoingWorld() const;
    // Shows the last recorded load failure to the user: the loader's message,
    // the project-relative file:line, and what is in the world now. Called on
    // the failure branch of every open path.
    void ShowLoadFailure(LoadFailureKind kind, const OutgoingWorld& outgoing);
    void PromptIfDirtyThen(PendingAction next);
    // Show the "Restore unsaved changes?" prompt for a scene whose open found
    // crash-recovery staging. The chosen resolution re-queues as a pending
    // action (opens never run inside UI callbacks).
    void BeginRecoveryPrompt(const std::filesystem::path& scenePath);
    void DoSave();
    // Save that has already cleared (or does not need) the degraded-document guard.
    void DoSaveConfirmed(DegradedSavePolicy degradedPolicy);
    // Explains what saving a degraded scene would cost and offers Save Anyway / Save As / Cancel.
    void PromptDegradedThenSave();
    void DoSaveAsFlow();
    // Eager persistence only: if the active scene has no file (the seeded boot
    // scene, or an opened untitled scene), prompt once to name and save it so
    // auto-save can run. A no-op while a scene build, play mode, or another save
    // prompt is in progress.
    void MaybePromptSaveUntitledScene();
    // True (and reports kSaveBlockedDuringPlayMessage to the log and to *outError)
    // when the document refuses saves because play mode is running. Called at the
    // manual entry points so the refusal surfaces where the user acted; the
    // document's own guard is what makes the refusal binding.
    bool RefuseSaveDuringPlay(std::string* outError) const;
    void PollAutoSavePreferences();
    void AddRecentScenePath(const std::filesystem::path& path) const;

    ECS::World* m_World = nullptr; // not owned
    Editor::EditorChangeNotifications* m_Notifications = nullptr; // not owned
    Editor::UndoRedoService* m_UndoRedo = nullptr; // not owned

    std::unique_ptr<Editor::SceneDocumentManager> m_Doc;
    // Survives until AttachToRoot builds m_Doc, which is where it lands.
    std::function<void()> m_SceneScopedGeneratorRelease;
    // Same hold-until-AttachToRoot contract as the release above.
    std::function<bool()> m_PlayModeProbe;
    SaveSceneChangesModal* m_SaveChangesModal = nullptr; // owned by UI tree
    SaveSceneAsModal* m_SaveAsModal = nullptr; // owned by UI tree
    RestoreSceneBackupModal* m_RecoveryModal = nullptr; // owned by UI tree
    ConfirmActionModal* m_RevertConfirmModal = nullptr; // owned by UI tree
    ConfirmActionModal* m_LoadFailedModal = nullptr; // owned by UI tree; notice mode

    std::optional<PendingAction> m_Pending;
    std::optional<PendingAction> m_AfterPrompt;
    bool m_IgnoreDirtyForNextPending = false;

    // Web scene-persistence prompts. Auto-save only runs once a scene has a file
    // (TickAutoSave gates on HasScenePath), so on web New Scene names the scene up
    // front, and an untitled scene reached any other way (the seeded boot scene, an
    // opened untitled scene) is prompted once to save. See RequestNewScene() and
    // MaybePromptSaveUntitledScene().
    bool m_EagerScenePersistence = false;
    bool m_PendingNewSceneNaming = false;
    bool m_UntitledSavePromptActive = false;
    bool m_UntitledSavePromptShownForEpisode = false;
    std::function<bool()> m_BootFlowActiveProbe;

    // Backup scene consumed by the next OpenRecovered pending action.
    std::filesystem::path m_RecoveredBackupScenePath;

    float m_AutoSaveAccumSeconds = 0.0f;
    float m_AutoSavePrefsPollAccum = 0.0f;
    bool m_AutoSaveEnabledCached = true;
    float m_AutoSaveIntervalCachedSec = 120.0f;

    std::function<void(const std::filesystem::path&)> m_OnAfterSave;

    /// When non-empty, invoked after a successful Save As (used for quit flow).
    std::function<void()> m_DeferredQuitAfterSave;
};

} // namespace Editor
} // namespace GameEngine

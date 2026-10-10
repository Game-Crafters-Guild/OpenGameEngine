#include "Scene/SceneEditorController.h"

#include "EditorInputActions.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/RestoreSceneBackupModal.h"
#include "Panels/SaveSceneAsModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Scene/SceneBackupRecovery.h"
#include "Scene/SceneDocumentManager.h"
#include "Scene/WorldSnapshotCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Input/InputSystem.h"
#include "Logger/Logger.h"
#include "TerrainECS/TerrainService.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <system_error>
#include <unordered_set>

namespace GameEngine::Editor
{
namespace
{
constexpr float kAutoSavePrefsPollSeconds = 1.0f;
constexpr double kAutoSaveIntervalMinSec = 30.0;
constexpr double kAutoSaveIntervalMaxSec = 300.0;
constexpr double kAutoSaveIntervalDefaultSec = 120.0;
constexpr size_t kMaxRecentScenes = 10;
constexpr const char* kRecentScenesPrefsKey = "recentScenes";

// Per-frame wall-clock budget for the deferred scene-build resolve. Bounds how
// long PumpSceneBuild spends resolving entities each frame before yielding so the
// editor presents a frame — trades load wall-clock for frame cadence. Each pumped
// frame still pays its full render (~50-90ms for a heavy scene), so a *smaller*
// budget renders more intermediate frames but inflates total load time (more
// render passes over the same resolve work). Measured on ElvenRealm (21k
// entities): 100ms budget pumped ~2fps but ran +36% wall-clock over the old
// synchronous freeze; 250ms pumps ~3fps and keeps wall-clock within ~10% of it —
// the resolve work dominates each frame, render is the minority, load stays
// interactive. Raise it toward smoother wall-clock, lower it toward more frames.
constexpr std::chrono::milliseconds kSceneBuildFrameBudget{250};

std::filesystem::path NormalizeRecentScenePath(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    std::error_code ec;
    std::filesystem::path normalized = path;
    if (!normalized.is_absolute())
        normalized = std::filesystem::absolute(normalized, ec);
    normalized = normalized.lexically_normal();

    std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec && !canonical.empty())
        normalized = canonical.lexically_normal();

    return normalized;
}

std::string NormalizeRecentSceneKey(const std::filesystem::path& path)
{
    std::string key = NormalizeRecentScenePath(path).string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

bool IsSceneFilePath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".scene";
}

// "Assets/Scenes/dupes.scene:6": the file relative to the open project, which
// is how the user finds it in the Assets panel, or absolute when it lies outside
// the project. The log keeps the absolute path.
std::string DescribeLoadFailureLocation(const std::filesystem::path& file, int line)
{
    const std::filesystem::path relative =
        RelativePathUnderRoot(file, EngineCore::GetInstance().GetWorkspaceRoot());
    std::string location = (relative.empty() ? file : relative).generic_string();
    if (line > 0)
        location += ":" + std::to_string(line);
    return location;
}
} // namespace

SceneEditorController::SceneEditorController() = default;

SceneEditorController::~SceneEditorController()
{
    // Normal teardown is a clean close: any unsaved work was saved or explicitly
    // discarded through the quit prompt, so the autosave staging is stale. Only
    // a crash skips this — exactly the case recovery staging exists for.
    CleanupBackupsForCleanClose();
}

void SceneEditorController::AttachToRoot(UIElement& root,
                                        ECS::World& world,
                                        EditorChangeNotifications& notifications,
                                        UndoRedoService* undoRedo)
{
    m_World = &world;
    m_Notifications = &notifications;
    m_UndoRedo = undoRedo;

    // UI modals
    {
        auto saveChanges = std::make_unique<SaveSceneChangesModal>();
        m_SaveChangesModal = saveChanges.get();
        root.AddChild(std::move(saveChanges));

        auto saveAs = std::make_unique<SaveSceneAsModal>();
        m_SaveAsModal = saveAs.get();
        root.AddChild(std::move(saveAs));

        auto recovery = std::make_unique<RestoreSceneBackupModal>();
        m_RecoveryModal = recovery.get();
        root.AddChild(std::move(recovery));

        auto loadFailed = std::make_unique<ConfirmActionModal>();
        m_LoadFailedModal = loadFailed.get();
        root.AddChild(std::move(loadFailed));

        auto revertConfirm = std::make_unique<ConfirmActionModal>();
        m_RevertConfirmModal = revertConfirm.get();
        root.AddChild(std::move(revertConfirm));
        m_RevertConfirmModal->SetOnConfirm([this]() {
            if (!m_Doc)
                return;
            // RequestRevertScene refuses before this modal can open, so play
            // mode should be unreachable here. Asked anyway: the dialog
            // can outlive the state it opened in.
            if (m_Doc->IsSaveBlockedByPlayMode())
            {
                Logger::Log::Error("SceneEditor: cannot revert the scene until play mode returns to Edit");
                return;
            }
            const auto path = m_Doc->GetScenePath();
            if (!path || path->empty())
            {
                // Same discard flag as Save Changes → Don't Save, so Update()
                // does not re-prompt before executing the pending NewScene.
                m_IgnoreDirtyForNextPending = true;
                RequestNewScene();
                return;
            }
            RequestOpenScene(*path, /*additive*/ false, OpenRestorePolicy::Discard);
        });
    }

    // Document manager
    m_Doc = std::make_unique<SceneDocumentManager>();
    m_Doc->SetWorld(m_World);
    m_Doc->SetChangeNotifications(m_Notifications);
    m_Doc->MarkClean();
    // Supplied before this point: the owner wires it at service-construction
    // time, which runs well ahead of AttachToRoot.
    if (m_SceneScopedGeneratorRelease)
        m_Doc->SetSceneScopedGeneratorRelease(m_SceneScopedGeneratorRelease);
    if (m_PlayModeProbe)
        m_Doc->SetPlayModeProbe(m_PlayModeProbe);

    if (m_SaveAsModal)
    {
        m_SaveAsModal->SetOnSave([this](const std::filesystem::path& path)
                                 {
                                     if (!m_Doc)
                                         return;
                                     // DoSaveAsFlow refuses before this modal can open, so play
                                     // mode should be unreachable here. Asked anyway: the dialog
                                     // can outlive the state it opened in.
                                     if (RefuseSaveDuringPlay(nullptr))
                                         return;
                                     if (m_EagerScenePersistence)
                                     {
                                         m_UntitledSavePromptActive = false;
                                         if (m_PendingNewSceneNaming)
                                         {
                                             // A name for a brand-new scene: start fresh under it. The
                                             // current scene is discarded — cancel was the way to keep it.
                                             m_PendingNewSceneNaming = false;
                                             CleanupBackupsForCleanClose();
                                             m_Doc->NewSceneUntitled();
                                             if (m_UndoRedo)
                                                 m_UndoRedo->Clear();
                                             if (m_Doc->SaveAs(path))
                                             {
                                                 AddRecentScenePath(path);
                                                 if (m_OnAfterSave)
                                                     m_OnAfterSave(path);
                                             }
                                             m_UntitledSavePromptShownForEpisode = false;
                                             return;
                                         }
                                     }
                                     if (!m_Doc->SaveAs(path))
                                         return;
                                     AddRecentScenePath(path);
                                     if (m_OnAfterSave)
                                         m_OnAfterSave(path);
                                     if (m_AfterPrompt.has_value())
                                     {
                                         m_Pending = *m_AfterPrompt;
                                         m_AfterPrompt.reset();
                                     }
                                     if (m_DeferredQuitAfterSave)
                                     {
                                         auto quit = std::move(m_DeferredQuitAfterSave);
                                         m_DeferredQuitAfterSave = {};
                                         if (quit)
                                             quit();
                                     }
                                 });
        m_SaveAsModal->SetOnCancel([this]() {
            m_AfterPrompt.reset();
            m_DeferredQuitAfterSave = {};
            if (m_EagerScenePersistence)
            {
                // New-scene cancel keeps the current scene; untitled-prompt cancel just
                // declines to save now. Both stop re-prompting until the next episode.
                m_PendingNewSceneNaming = false;
                m_UntitledSavePromptActive = false;
            }
        });
    }

    PollAutoSavePreferences();
}

void SceneEditorController::Update(Input::InputSystem* inputSystem)
{
    if (!m_Doc)
        return;

    // Q6 slice 3b: recover baked HLOD proxies after an in-place device rebuild
    // (no-op unless the device's rebuild generation moved since last frame).
    m_Doc->PollDeviceRebuildAndRecoverHlod();

    if (inputSystem && !Editor::ShouldSuppressEditorShortcutActions())
    {
        if (inputSystem->WasActionTriggered(EditorInput::kEditorSaveSceneAs))
        {
            RequestSaveSceneAs();
        }
        else if (inputSystem->WasActionTriggered(EditorInput::kEditorSaveScene))
        {
            RequestSaveScene();
        }
    }

    if (m_Pending.has_value() && m_Pending->kind != PendingAction::Kind::None)
    {
        PendingAction a = *m_Pending;
        m_Pending.reset();

        // If the user already chose "Don't Save" for this pending action,
        // do not prompt again in the next frame (otherwise we loop forever).
        // A non-Prompt restore policy (headless automation) answers the dirty
        // prompt as Don't Save without showing it.
        const bool policyAnswersDirtyPrompt =
            a.restorePolicy != OpenRestorePolicy::Prompt;
        const bool shouldPromptDirty = IsSceneDirty() && m_SaveChangesModal
                                    && !m_IgnoreDirtyForNextPending && !policyAnswersDirtyPrompt;
        if (shouldPromptDirty)
        {
            PromptIfDirtyThen(a);
        }
        else
        {
            m_IgnoreDirtyForNextPending = false;
            Execute(a);
        }
    }

    // Drive the deferred scene-build resolve. Execute() above may have begun a
    // build this frame (OpenSceneReplace defers the per-entity GPU resolve); this
    // spends a bounded slice on it and yields, so the frame still renders and the
    // scene materializes progressively over subsequent frames. No-op when idle.
    if (m_Doc)
        m_Doc->PumpSceneBuild(kSceneBuildFrameBudget);

    MaybePromptSaveUntitledScene();
}

bool SceneEditorController::IsSceneBuildInProgress() const
{
    return m_Doc && m_Doc->IsSceneBuildInProgress();
}

void SceneEditorController::DrainSceneBuild()
{
    if (m_Doc)
        m_Doc->DrainSceneBuild();
}

bool SceneEditorController::GetSceneBuildProgress(uint64_t& outProcessed, uint64_t& outTotal) const
{
    return m_Doc && m_Doc->GetSceneBuildProgress(outProcessed, outTotal);
}

bool SceneEditorController::HandleAssetOpen(const std::filesystem::path& path)
{
    if (path.empty())
        return false;
    std::string ext = path.extension().string();
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".scene")
        return false;

    Logger::Log::Info("SceneEditor: request open scene '{}'", path.string());
    RequestOpenScene(path, /*additive*/ false);
    return true;
}

void SceneEditorController::RequestOpenScene(const std::filesystem::path& path, bool additive,
                                             OpenRestorePolicy restorePolicy)
{
    if (path.empty())
        return;
    // Relative paths (IPC/scripted opens) resolve through the same implicit
    // source priority every other project-file reference uses — project mount
    // first, then registered sources such as 'editor' — never the process CWD.
    // Absolute paths pass through unchanged. Resolving at this single funnel
    // keeps the downstream open machinery (dirty prompt, crash-recovery scan,
    // document path, recent-scenes list) on one canonical absolute path for
    // UI and IPC alike.
    const std::filesystem::path resolved =
        EngineCore::GetInstance().GetAssetManager().ResolveAssetPath(path);
    PendingAction a{};
    a.kind = additive ? PendingAction::Kind::OpenAdditive : PendingAction::Kind::OpenReplace;
    a.path = resolved.empty() ? path : resolved;
    a.restorePolicy = restorePolicy;
    m_Pending = a;
}

std::vector<std::uint8_t> SceneEditorController::CaptureWorldSnapshot() const
{
    if (!m_Doc)
        return {};
    return m_Doc->CaptureWorldSnapshot();
}

bool SceneEditorController::ApplyWorldSnapshot(const std::vector<std::uint8_t>& snapshot) const
{
    if (!m_Doc)
        return false;
    return m_Doc->ApplyWorldSnapshot(snapshot);
}

std::optional<std::filesystem::path> SceneEditorController::GetActiveScenePath() const
{
    if (!m_Doc)
        return std::nullopt;
    return m_Doc->GetScenePath();
}

std::optional<std::string> SceneEditorController::GetActiveSceneDisplayName() const
{
    if (!m_Doc)
        return std::nullopt;
    return m_Doc->GetDisplaySceneName();
}

const std::optional<SceneLoadFailure>& SceneEditorController::GetLastSceneLoadFailure() const
{
    static const std::optional<SceneLoadFailure> kNoDocument{};
    return m_Doc ? m_Doc->GetLastLoadFailure() : kNoDocument;
}

const std::optional<SceneLoadDegraded>& SceneEditorController::GetLastSceneLoadDegraded() const
{
    static const std::optional<SceneLoadDegraded> kNoDocument{};
    return m_Doc ? m_Doc->GetLastLoadDegraded() : kNoDocument;
}

std::optional<SceneLoadDegraded> SceneEditorController::GetOutstandingSceneLoadDegraded() const
{
    return m_Doc ? m_Doc->GetOutstandingLoadDegraded() : std::nullopt;
}

std::vector<std::filesystem::path> SceneEditorController::GetRecentScenePaths() const
{
    std::vector<std::filesystem::path> scenes;

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    const auto& root = prefs.Json();
    const auto it = root.find(kRecentScenesPrefsKey);
    if (it == root.end() || !it->is_array())
        return scenes;

    std::unordered_set<std::string> seen;
    for (const auto& item : *it)
    {
        if (!item.is_string())
            continue;

        const std::filesystem::path normalized = NormalizeRecentScenePath(
            std::filesystem::path(item.get<std::string>()));
        if (normalized.empty() || !IsSceneFilePath(normalized))
            continue;

        std::error_code ec;
        if (!std::filesystem::exists(normalized, ec) || !std::filesystem::is_regular_file(normalized, ec))
            continue;

        const std::string key = NormalizeRecentSceneKey(normalized);
        if (key.empty() || !seen.insert(key).second)
            continue;

        scenes.push_back(normalized);
        if (scenes.size() >= kMaxRecentScenes)
            break;
    }

    return scenes;
}

bool SceneEditorController::IsSceneDirty() const
{
    if (!m_Doc)
        return false;
    if (m_Doc->IsDirty())
        return true;
    // Brush strokes mutate zone payloads / the sphere sculpt store, never ECS
    // components, so the document flag alone misses stroke-only sessions. The
    // quit/switch prompts must fire for them: autosave never touches the live
    // .tzone/.tsculpt files, so closing without saving genuinely discards them.
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
        return terrain->AnyZonePayloadNeedsSave() || terrain->SphereSculptNeedsSave();
    return false;
}

void SceneEditorController::MarkSceneDirty()
{
    if (m_Doc)
        m_Doc->MarkDirty();
}

bool SceneEditorController::IsAnyModalOpen() const
{
    return (m_SaveAsModal && m_SaveAsModal->IsVisible())
        || (m_SaveChangesModal && m_SaveChangesModal->IsVisible())
        || (m_RecoveryModal && m_RecoveryModal->IsVisible())
        || (m_RevertConfirmModal && m_RevertConfirmModal->IsVisible())
        || (m_LoadFailedModal && m_LoadFailedModal->IsVisible());
}

std::string SceneEditorController::GetActiveModalKind() const
{
    if (m_SaveChangesModal && m_SaveChangesModal->IsVisible())
        return "saveChanges";
    if (m_RecoveryModal && m_RecoveryModal->IsVisible())
        return "restoreBackup";
    if (m_SaveAsModal && m_SaveAsModal->IsVisible())
        return "saveAs";
    if (m_RevertConfirmModal && m_RevertConfirmModal->IsVisible())
        return "revertScene";
    if (m_LoadFailedModal && m_LoadFailedModal->IsVisible())
        return "loadFailed";
    return {};
}

bool SceneEditorController::RespondToActiveModal(const std::string& choice, std::string* outError)
{
    auto fail = [outError](const std::string& msg)
    {
        if (outError)
            *outError = msg;
        return false;
    };

    if (m_SaveChangesModal && m_SaveChangesModal->IsVisible())
    {
        if (choice == "save")
            m_SaveChangesModal->ChooseSave();
        else if (choice == "dontSave")
            m_SaveChangesModal->ChooseDontSave();
        else if (choice == "cancel")
            m_SaveChangesModal->ChooseCancel();
        else
            return fail("saveChanges modal accepts: save, dontSave, cancel");
        return true;
    }
    if (m_RecoveryModal && m_RecoveryModal->IsVisible())
    {
        if (choice == "restore")
            m_RecoveryModal->ChooseRestore();
        else if (choice == "discard")
            m_RecoveryModal->ChooseDiscard();
        else
            return fail("restoreBackup modal accepts: restore, discard");
        return true;
    }
    if (m_SaveAsModal && m_SaveAsModal->IsVisible())
    {
        if (choice == "cancel")
            m_SaveAsModal->ChooseCancel();
        else
            return fail("saveAs modal accepts: cancel (drive Save through save_scene with a path)");
        return true;
    }
    if (m_RevertConfirmModal && m_RevertConfirmModal->IsVisible())
    {
        if (choice == "confirm")
            m_RevertConfirmModal->ChooseConfirm();
        else if (choice == "cancel")
            m_RevertConfirmModal->ChooseCancel();
        else
            return fail("revertScene modal accepts: confirm, cancel");
        return true;
    }
    if (m_LoadFailedModal && m_LoadFailedModal->IsVisible())
    {
        if (choice != "ok")
            return fail("loadFailed modal accepts: ok");
        m_LoadFailedModal->ChooseConfirm();
        return true;
    }
    return fail("No scene modal is visible");
}

void SceneEditorController::SetHierarchyUiCaptureForSave(std::function<Scene::SceneHierarchyUi()> capture)
{
    if (!m_Doc)
        return;
    m_Doc->SetHierarchyUiCaptureForSave(std::move(capture));
}

std::optional<Scene::SceneHierarchyUiFromFile> SceneEditorController::ConsumeHierarchyUiFromLastDocumentLoad()
{
    if (!m_Doc)
        return std::nullopt;
    return m_Doc->ConsumeHierarchyUiFromLastDocumentLoad();
}

void SceneEditorController::SetEditorCameraCaptureForSave(std::function<Scene::SceneEditorCamera()> capture)
{
    if (!m_Doc)
        return;
    m_Doc->SetEditorCameraCaptureForSave(std::move(capture));
}

std::optional<Scene::SceneEditorCameraFromFile> SceneEditorController::ConsumeEditorCameraFromLastDocumentLoad()
{
    if (!m_Doc)
        return std::nullopt;
    return m_Doc->ConsumeEditorCameraFromLastDocumentLoad();
}

void SceneEditorController::SetSceneScopedGeneratorRelease(std::function<void()> release)
{
    // Held here, not just forwarded: the owner wires this at service-construction
    // time, before AttachToRoot builds m_Doc. Dropping it when m_Doc is absent
    // (what every other setter here does) silently disarms the release, and an
    // unarmed release is indistinguishable from a working one until a scene swap
    // destroys entities of the incoming scene.
    m_SceneScopedGeneratorRelease = std::move(release);
    if (m_Doc)
        m_Doc->SetSceneScopedGeneratorRelease(m_SceneScopedGeneratorRelease);
}

void SceneEditorController::DropUndoHistoryIfTheWorldWasCleared()
{
    if (!m_UndoRedo || !m_Doc)
        return;

    const auto& failure = m_Doc->GetLastLoadFailure();
    if (!failure || !failure->WorldCleared)
        return;

    // The open failed, but not before World::Clear destroyed the world every
    // entry on this stack was recorded against. The entries are worse than
    // stale: Clear recycles entity indices and restarts versions, so their
    // EntityHandles pass IsValid against unrelated entities of the partial load
    // (SceneSwapOrdering.RetireAfterClearDestroysAnEntityOfTheIncomingScene),
    // and a snapshot entry would restore the destroyed world straight over it.
    // The successful swap sites clear for the same reason.
    m_UndoRedo->Clear();
}

SceneEditorController::OutgoingWorld SceneEditorController::CaptureOutgoingWorld() const
{
    OutgoingWorld outgoing{};
    if (!m_Doc)
        return outgoing;
    if (const auto path = m_Doc->GetScenePath())
        outgoing.SceneName = path->filename().string();
    else if (const auto& partial = m_Doc->GetPartialLoadDocument())
        outgoing.PartialLoadOf = partial->filename().string();
    return outgoing;
}

void SceneEditorController::ShowLoadFailure(LoadFailureKind kind, const OutgoingWorld& outgoing)
{
    if (!m_LoadFailedModal || !m_Doc)
        return;
    const auto& failure = m_Doc->GetLastLoadFailure();
    if (!failure)
        return;

    const std::string name = failure->DocumentPath.filename().string();
    const std::string entities = FormatCount(failure->EntitiesInWorld, "entity", "entities");
    // The world before the open, as the subject of a sentence: the document's
    // file, the partial load a previous failed open left, or an untitled scene.
    const std::string before = !outgoing.SceneName.empty()      ? outgoing.SceneName
                               : !outgoing.PartialLoadOf.empty() ? "The partial load of " + outgoing.PartialLoadOf
                                                                 : std::string("The untitled scene");

    // What the user is looking at now. The record knows whether the clear ran;
    // only the caller knows whether the open was additive or a crash recovery,
    // and what the world held before it.
    ConfirmActionModal::Notice notice{};
    if (failure->WorldCleared)
    {
        notice.Title = "Scene loaded only in part";
        const char* gone = outgoing.PartialLoadOf.empty() ? " closed" : " was cleared";
        notice.Closing = name + " was only partly loaded. " + before + gone + " before the error. "
                         "The world now holds " + entities + " from " + name +
                         ". The file on disk is unchanged.";
    }
    else if (kind == LoadFailureKind::Additive)
    {
        notice.Title = "Scene could not be added";
        notice.Closing = name + " was not added. " + before + " stays as it was.";
    }
    else if (kind == LoadFailureKind::AdditivePartial)
    {
        notice.Title = "Scene added only in part";
        notice.Closing = name + " was only partly added. The entities it created before the error stay in "
                         "the world.";
    }
    else if (!outgoing.PartialLoadOf.empty())
    {
        // Nothing to "stay open": the world holds what an earlier failed open
        // left, and still does.
        notice.Title = "Scene failed to load";
        notice.Closing = name + " did not load. The world still holds " + entities + " from " +
                         outgoing.PartialLoadOf + ".";
    }
    else
    {
        notice.Title = "Scene failed to load";
        notice.Closing = name + " did not load. " + before + " stays open.";
    }
    if (kind == LoadFailureKind::Recovered)
        notice.Closing += " The autosave backup was left in place.";

    notice.Message = failure->Message.empty() ? "The loader reported no reason." : failure->Message;
    notice.Detail = DescribeLoadFailureLocation(failure->ErrorFile, failure->ErrorLine);
    notice.DismissLabel = "OK";
    m_LoadFailedModal->ShowNotice(notice);
}

void SceneEditorController::Execute(const PendingAction& a)
{
    if (!m_Doc)
        return;

    // Every action here replaces or adds to the world, so a failure notice still
    // on screen from an earlier open no longer describes it.
    if (m_LoadFailedModal)
        m_LoadFailedModal->Hide();

    switch (a.kind)
    {
    case PendingAction::Kind::NewScene:
        // The outgoing document closes cleanly here (any dirty state was
        // resolved by the prompt in Update), so its autosave staging is stale.
        CleanupBackupsForCleanClose();
        m_Doc->NewSceneUntitled();
        if (m_UndoRedo)
            m_UndoRedo->Clear();
        return;
    case PendingAction::Kind::OpenReplace:
    {
        CleanupBackupsForCleanClose();
        // Crash recovery: leftover autosave staging for the incoming scene means
        // the previous session ended uncleanly. Ask before opening — the answer
        // re-queues as OpenRecovered (restore) or a plain OpenReplace (discard).
        // A non-Prompt policy resolves the same decision headlessly, mirroring
        // the modal callbacks (opens always run via a re-queued pending action).
        const SceneRecoveryScan scan = ScanForSceneRecovery(a.path);
        if (scan.RecoveryPending())
        {
            if (a.restorePolicy != OpenRestorePolicy::Prompt)
            {
                const bool restore = a.restorePolicy == OpenRestorePolicy::Restore;
                const SceneRecoveryResolution r = ResolveRecoveryDecision(scan, restore);
                PendingAction next{};
                next.path = r.DocumentPath;
                if (r.OpenedFromBackup)
                {
                    next.kind = PendingAction::Kind::OpenRecovered;
                    m_RecoveredBackupScenePath = r.ContentPathToOpen;
                }
                else
                {
                    next.kind = PendingAction::Kind::OpenReplace;
                }
                m_IgnoreDirtyForNextPending = true;
                m_Pending = next;
                return;
            }
            if (m_RecoveryModal)
            {
                BeginRecoveryPrompt(a.path);
                return;
            }
        }
        const OutgoingWorld outgoing = CaptureOutgoingWorld();
        if (m_Doc->OpenSceneReplace(a.path))
        {
            AddRecentScenePath(a.path);
            // A replace-open builds a brand-new world; the outgoing scene's undo
            // history no longer applies to it (mirrors NewScene). Without this, a
            // Ctrl+Z during the new scene's deferred build would restore the OLD
            // scene's snapshot over the half-built new one. NewScene clears above.
            if (m_UndoRedo)
                m_UndoRedo->Clear();
        }
        else
        {
            DropUndoHistoryIfTheWorldWasCleared();
            ShowLoadFailure(LoadFailureKind::Replace, outgoing);
        }
        return;
    }
    case PendingAction::Kind::OpenRecovered:
    {
        const std::filesystem::path backup = m_RecoveredBackupScenePath;
        m_RecoveredBackupScenePath.clear();
        const OutgoingWorld outgoing = CaptureOutgoingWorld();
        if (m_Doc->OpenSceneRecovered(backup, a.path))
        {
            AddRecentScenePath(a.path);
            if (m_UndoRedo)
                m_UndoRedo->Clear();
        }
        else
        {
            DropUndoHistoryIfTheWorldWasCleared();
            // Deliberately silent about the world: whether the outgoing scene
            // survived is the recorded failure's story, and RecordLoadFailure
            // has already announced it at Error with the entity count.
            Logger::Log::Warning("SceneEditor: recovery open of '{}' failed, so '{}' was not opened "
                                 "and its backup file was left in place",
                                 backup.string(), a.path.string());
            ShowLoadFailure(LoadFailureKind::Recovered, outgoing);
        }
        return;
    }
    case PendingAction::Kind::OpenAdditive:
    {
        if (!m_World)
        {
            if (m_Doc->LoadSceneAdditive(a.path))
                AddRecentScenePath(a.path);
            return;
        }

        // Resolve any in-flight replace-open build FIRST, so `before` captures a
        // fully-resolved world. LoadSceneAdditive below cancels the pump, so
        // without this `before` would snapshot unresolved entities while `after`
        // (post sync-resolve) has them resolved — undo would then mass-revert them
        // to meshGpuHandleId==0 and hide them forever.
        if (m_Doc->IsSceneBuildInProgress())
            m_Doc->DrainSceneBuild();

        const OutgoingWorld outgoing = CaptureOutgoingWorld();
        const auto before = m_World->SerializeWorld();
        const bool loaded = m_Doc->LoadSceneAdditive(a.path);
        const auto after = m_World->SerializeWorld();
        if (loaded)
            AddRecentScenePath(a.path);
        if (m_UndoRedo && before != after)
        {
            auto cmd = std::make_unique<WorldSnapshotCommand>(
                "Load Scene Additive",
                m_World,
                m_Notifications,
                before,
                after);
            m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
        }
        if (!loaded)
            ShowLoadFailure(before != after ? LoadFailureKind::AdditivePartial : LoadFailureKind::Additive,
                            outgoing);
        return;
    }
    default:
        return;
    }
}

void SceneEditorController::BeginRecoveryPrompt(const std::filesystem::path& scenePath)
{
    if (!m_RecoveryModal)
        return;

    // Both answers re-queue the open as a pending action: opens never run inside
    // UI callbacks (same deferral as RequestOpenScene). The dirty prompt already
    // resolved before Execute reached the scan, so skip it on the re-queue.
    m_RecoveryModal->SetOnRestore([this, scenePath]()
    {
        const SceneRecoveryResolution r =
            ResolveRecoveryDecision(ScanForSceneRecovery(scenePath), /*restore*/ true);
        PendingAction a{};
        a.path = r.DocumentPath;
        if (r.OpenedFromBackup)
        {
            a.kind = PendingAction::Kind::OpenRecovered;
            m_RecoveredBackupScenePath = r.ContentPathToOpen;
        }
        else
        {
            // Payload-only recovery: the staged payloads were promoted over the
            // live .tzone files; the scene structure itself had no newer backup.
            a.kind = PendingAction::Kind::OpenReplace;
        }
        m_IgnoreDirtyForNextPending = true;
        m_Pending = a;
    });
    m_RecoveryModal->SetOnDiscard([this, scenePath]()
    {
        const SceneRecoveryResolution r =
            ResolveRecoveryDecision(ScanForSceneRecovery(scenePath), /*restore*/ false);
        PendingAction a{};
        a.kind = PendingAction::Kind::OpenReplace;
        a.path = r.DocumentPath;
        m_IgnoreDirtyForNextPending = true;
        m_Pending = a;
    });

    m_RecoveryModal->Show("Restore unsaved changes?",
                          "\"" + scenePath.filename().string()
                              + "\" has autosaved changes from a session that did not close cleanly.\n"
                                "Restore them, or discard them and open the last saved version?");
}

void SceneEditorController::CleanupBackupsForCleanClose()
{
    if (!m_Doc)
        return;
    const auto& scenePath = m_Doc->GetScenePath();
    if (!scenePath.has_value() || scenePath->empty())
        return;
    DiscardSceneBackups(ScanForSceneRecovery(*scenePath));
}

void SceneEditorController::PromptIfDirtyThen(PendingAction next)
{
    if (!m_Doc || !m_SaveChangesModal)
        return;

    m_AfterPrompt = next;

    const std::string msg = m_Doc->HasScenePath()
                                ? "The current scene has unsaved changes. Save before changing scenes?"
                                : "The current scene has unsaved changes and has not been saved yet. Save before changing scenes?";

    m_SaveChangesModal->SetOnSave([this]()
                                 {
                                     if (!m_Doc)
                                         return;
                                     // The document would refuse this write anyway; asking here is
                                     // what states the reason instead of the scene change simply
                                     // not happening.
                                     if (RefuseSaveDuringPlay(nullptr))
                                         return;
                                     if (m_Doc->HasScenePath())
                                     {
                                         if (m_Doc->Save())
                                         {
                                             if (auto scenePath = m_Doc->GetScenePath())
                                                 AddRecentScenePath(*scenePath);
                                             if (m_OnAfterSave)
                                             {
                                                 if (auto scenePath = m_Doc->GetScenePath())
                                                     m_OnAfterSave(*scenePath);
                                             }
                                             if (m_AfterPrompt.has_value())
                                             {
                                                 m_Pending = *m_AfterPrompt;
                                                 m_AfterPrompt.reset();
                                             }
                                         }
                                         return;
                                     }
                                     DoSaveAsFlow();
                                 });
    m_SaveChangesModal->SetOnDontSave([this]()
                                     {
                                         if (m_AfterPrompt.has_value())
                                         {
                                             // Mark that we should execute the next pending action even though the
                                             // document is still dirty (user explicitly chose to discard changes).
                                             m_IgnoreDirtyForNextPending = true;
                                             m_Pending = *m_AfterPrompt;
                                             m_AfterPrompt.reset();
                                         }
                                     });
    m_SaveChangesModal->SetOnCancel([this]()
                                    {
                                        m_AfterPrompt.reset();
                                        m_IgnoreDirtyForNextPending = false;
                                    });

    m_SaveChangesModal->Show("Save changes?", msg);
}

void SceneEditorController::PromptSaveBeforeQuit(std::function<void()> proceedQuit)
{
    if (!proceedQuit)
        return;
    if (!m_Doc || !m_SaveChangesModal)
    {
        proceedQuit();
        return;
    }
    // Payload-aware: stroke-only sessions must prompt too (see IsSceneDirty).
    if (!IsSceneDirty())
    {
        proceedQuit();
        return;
    }

    const std::string msg =
        m_Doc->HasScenePath()
            ? "Save changes to the scene before quitting?"
            : "The scene has unsaved changes and has not been saved yet. Save before quitting?";

    auto finishHeld = std::make_shared<std::function<void()>>(std::move(proceedQuit));
    auto finishQuit = [finishHeld]() {
        if (!finishHeld || !*finishHeld)
            return;
        std::function<void()> fn = std::move(*finishHeld);
        *finishHeld = nullptr;
        if (fn)
            fn();
    };

    m_SaveChangesModal->SetOnSave([this, finishQuit]() {
        if (!m_Doc)
            return;
        // Refused during play, and the quit does NOT proceed: finishQuit stays unrun, because
        // discarding unsaved authored work is not a reasonable reading of "Save". The user is
        // told what to do; Don't Save remains the deliberate way out.
        if (RefuseSaveDuringPlay(nullptr))
            return;
        if (m_Doc->HasScenePath())
        {
            if (m_Doc->Save())
            {
                if (auto scenePath = m_Doc->GetScenePath())
                    AddRecentScenePath(*scenePath);
                if (m_OnAfterSave)
                {
                    if (auto scenePath = m_Doc->GetScenePath())
                        m_OnAfterSave(*scenePath);
                }
                finishQuit();
            }
            return;
        }
        m_DeferredQuitAfterSave = finishQuit;
        DoSaveAsFlow();
    });
    m_SaveChangesModal->SetOnDontSave([finishQuit]() { finishQuit(); });
    m_SaveChangesModal->SetOnCancel([this, finishHeld]() {
        m_DeferredQuitAfterSave = {};
        if (finishHeld)
            *finishHeld = nullptr;
    });

    m_SaveChangesModal->Show("Save scene?", msg);
}

void SceneEditorController::SetPlayModeProbe(std::function<bool()> isNotEditing)
{
    m_PlayModeProbe = std::move(isNotEditing);
    if (m_Doc)
        m_Doc->SetPlayModeProbe(m_PlayModeProbe);
}

bool SceneEditorController::RefuseSaveDuringPlay(std::string* outError) const
{
    if (!m_Doc || !m_Doc->IsSaveBlockedByPlayMode())
        return false;
    Logger::Log::Error("SceneEditor: {}", kSaveBlockedDuringPlayMessage);
    if (outError)
        *outError = kSaveBlockedDuringPlayMessage;
    return true;
}

void SceneEditorController::DoSave()
{
    if (!m_Doc)
        return;
    // Play mode is refused BEFORE the degraded prompt can open: during play the write that
    // prompt exists to confirm is refused whichever button the user picks, so opening it would
    // ask a question with no answer. Play-only here — a degraded document in Edit mode must
    // still get its prompt rather than a flat refusal.
    if (RefuseSaveDuringPlay(nullptr))
        return;
    // A document that could not be fully read must not overwrite its own file on a reflex Ctrl+S.
    // Ask first, naming what would be lost; every other path is unchanged.
    if (m_Doc->HasScenePath() && m_Doc->OutstandingDegradedSkipCount() > 0 && m_SaveChangesModal)
    {
        PromptDegradedThenSave();
        return;
    }
    DoSaveConfirmed(DegradedSavePolicy::Refuse);
}

void SceneEditorController::DoSaveConfirmed(DegradedSavePolicy degradedPolicy)
{
    if (!m_Doc)
        return;
    if (m_Doc->HasScenePath())
    {
        if (!m_Doc->Save(degradedPolicy))
            return;
        const auto path = m_Doc->GetScenePath();
        if (path)
            AddRecentScenePath(*path);
        if (m_OnAfterSave)
        {
            if (path)
                m_OnAfterSave(*path);
        }
        return;
    }
    DoSaveAsFlow();
}

void SceneEditorController::DoSaveAsFlow()
{
    if (!m_SaveAsModal)
        return;
    // Refuse before the dialog opens: a name typed into a modal that is going to
    // be refused on commit is wasted work.
    if (RefuseSaveDuringPlay(nullptr))
        return;
    const auto& dir = EngineCore::GetInstance().GetResolvedAssetRoot();
    m_SaveAsModal->Show(dir);
}

void SceneEditorController::MaybePromptSaveUntitledScene()
{
    if (!m_EagerScenePersistence || !m_Doc || !m_SaveAsModal)
        return;
    if (m_Doc->HasScenePath())
    {
        // Saved: re-arm for the next untitled episode (a later New Scene or open).
        m_UntitledSavePromptShownForEpisode = false;
        return;
    }
    // One prompt per untitled episode; never stack on another flow or mid-build.
    if (m_UntitledSavePromptShownForEpisode || m_PendingNewSceneNaming ||
        m_UntitledSavePromptActive || m_SaveAsModal->IsVisible())
        return;
    if (m_Pending.has_value() || m_Doc->IsSceneBuildInProgress())
        return;
    // Never prompt while boot owns the scene: the picker's modal owns the screen
    // until a project is chosen, and a queued startup scene replaces the interim
    // empty scene the moment the world path settles.
    if (m_BootFlowActiveProbe && m_BootFlowActiveProbe())
        return;
    // m_PlayModeProbe() is true while play mode runs (not editing).
    if (m_PlayModeProbe && m_PlayModeProbe())
        return;

    m_UntitledSavePromptShownForEpisode = true;
    m_UntitledSavePromptActive = true;
    m_SaveAsModal->Show(EngineCore::GetInstance().GetResolvedAssetRoot());
}

void SceneEditorController::RequestNewScene()
{
    if (m_EagerScenePersistence)
    {
        // Name the new scene up front so it has a file the moment it exists (auto-save
        // needs one). Cancel keeps the current scene — nothing changes until a name is
        // committed. Handled in the Save As modal's OnSave/OnCancel (new-scene branch).
        if (m_SaveAsModal)
        {
            if (RefuseSaveDuringPlay(nullptr))
                return;
            m_PendingNewSceneNaming = true;
            m_SaveAsModal->Show(EngineCore::GetInstance().GetResolvedAssetRoot());
            return;
        }
    }
    PendingAction a{};
    a.kind = PendingAction::Kind::NewScene;
    m_Pending = a;
}

void SceneEditorController::RequestSaveScene()
{
    DoSave();
}

void SceneEditorController::RequestSaveSceneAs()
{
    DoSaveAsFlow();
}

void SceneEditorController::RequestRevertScene()
{
    if (!m_Doc || !m_RevertConfirmModal)
        return;
    // Reloading replaces the world play mode is simulating in place. Stop first.
    if (m_Doc->IsSaveBlockedByPlayMode())
    {
        Logger::Log::Error("SceneEditor: cannot revert the scene until play mode returns to Edit");
        return;
    }

    const auto path = m_Doc->GetScenePath();
    const bool untitled = !path || path->empty();
    // A clean untitled document is already a blank scene — nothing to discard.
    if (untitled && !IsSceneDirty())
        return;

    const std::string message = untitled
        ? "This scene has not been saved. Discard all changes?"
        : (IsSceneDirty()
               ? "Discard unsaved changes and reload \"" + path->filename().string() + "\" from disk?"
               : "Reload \"" + path->filename().string() + "\" from disk?");
    m_RevertConfirmModal->Show("Revert Scene?", message, "Revert");
}

void SceneEditorController::PromptDegradedThenSave()
{
    if (!m_Doc || !m_SaveChangesModal)
        return;
    // Outstanding, not load-time: the prompt's whole content is a claim about what this save is
    // about to do, and an override the user already discarded is no longer part of that.
    const std::optional<SceneLoadDegraded> degraded = m_Doc->GetOutstandingLoadDegraded();
    if (!degraded.has_value())
        return;

    const std::size_t dropped = degraded->DroppedCount();
    const std::size_t preserved = degraded->SkippedCount() - dropped;

    std::string msg = "This scene loaded with " + std::to_string(degraded->SkippedCount()) +
                      " assignment(s) this build could not read";
    if (dropped > 0)
    {
        msg += ", and saving will permanently drop " + std::to_string(dropped) + " of them from ";
        msg += degraded->DocumentPath.filename().string() + ".";
    }
    else
    {
        msg += ". Their original text was preserved and will be written back unchanged.";
    }
    if (preserved > 0 && dropped > 0)
        msg += " The other " + std::to_string(preserved) + " will be written back unchanged.";
    msg += " See the log for the full list.";

    m_SaveChangesModal->SetOnSave([this]() { DoSaveConfirmed(DegradedSavePolicy::SaveAnyway); });
    m_SaveChangesModal->SetOnDontSave([this]() { DoSaveAsFlow(); });
    m_SaveChangesModal->SetOnCancel([]() {});

    SaveSceneChangesModalLabels labels{};
    labels.Save = "Save Anyway";
    labels.DontSave = "Save As...";
    m_SaveChangesModal->Show("Save an incompletely loaded scene?", msg, labels);
}

bool SceneEditorController::SaveActiveScene(std::string* outError, DegradedSavePolicy degradedPolicy)
{
    if (!m_Doc)
    {
        if (outError)
            *outError = "scene editor not attached";
        return false;
    }
    if (RefuseSaveDuringPlay(outError))
        return false;
    if (!m_Doc->HasScenePath())
    {
        if (outError)
            *outError = "scene is untitled (provide a path to save-as)";
        return false;
    }
    // The guard's refusal is not a failure to report as one: say what is wrong and how to proceed,
    // because this string is what the debug server hands back to whoever asked for the save.
    if (const std::optional<SceneLoadDegraded> degraded = m_Doc->GetOutstandingLoadDegraded();
        degraded.has_value() && degradedPolicy == DegradedSavePolicy::Refuse)
    {
        if (outError)
            *outError = "scene loaded with " + std::to_string(degraded->SkippedCount()) +
                        " unreadable assignment(s) (" + std::to_string(degraded->DroppedCount()) +
                        " would be lost); pass force=true to save anyway, or save to a new path";
        return false;
    }
    if (!m_Doc->Save(degradedPolicy))
    {
        if (outError)
            *outError = "scene save failed";
        return false;
    }
    if (const auto path = m_Doc->GetScenePath())
    {
        AddRecentScenePath(*path);
        if (m_OnAfterSave)
            m_OnAfterSave(*path);
    }
    return true;
}

bool SceneEditorController::SaveActiveSceneAs(const std::filesystem::path& scenePath,
                                              std::string* outError,
                                              DegradedSavePolicy degradedPolicy)
{
    if (!m_Doc)
    {
        if (outError)
            *outError = "scene editor not attached";
        return false;
    }
    if (RefuseSaveDuringPlay(outError))
        return false;
    if (scenePath.empty())
    {
        if (outError)
            *outError = "empty scene path";
        return false;
    }
    // The degradation refusal is the only failure here with a remedy to offer. A disk error on a
    // perfectly healthy document reaches this same branch, and sending that user after force=true
    // points them at a guard that is not what stopped them.
    const bool refusedAsDegraded = m_Doc->WouldRefuseDegradedOverwrite(scenePath, degradedPolicy);
    if (!m_Doc->SaveAs(scenePath, degradedPolicy))
    {
        if (outError)
            *outError = refusedAsDegraded
                            ? "scene save-as refused: this document did not fully load, and this is "
                              "the file it was read from — save to a different path, or pass "
                              "force=true to overwrite it anyway"
                            : "scene save-as failed";
        return false;
    }
    AddRecentScenePath(scenePath);
    if (m_OnAfterSave)
        m_OnAfterSave(scenePath);
    return true;
}

void SceneEditorController::AddRecentScenePath(const std::filesystem::path& path) const
{
    const std::filesystem::path normalized = NormalizeRecentScenePath(path);
    if (normalized.empty() || !IsSceneFilePath(normalized))
        return;

    std::vector<std::filesystem::path> scenes = GetRecentScenePaths();
    const std::string key = NormalizeRecentSceneKey(normalized);
    scenes.erase(
        std::remove_if(scenes.begin(), scenes.end(),
                       [&key](const std::filesystem::path& existing)
                       {
                           return NormalizeRecentSceneKey(existing) == key;
                       }),
        scenes.end());
    scenes.insert(scenes.begin(), normalized);
    if (scenes.size() > kMaxRecentScenes)
        scenes.resize(kMaxRecentScenes);

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    nlohmann::json recentArray = nlohmann::json::array();
    for (const auto& scenePath : scenes)
        recentArray.push_back(scenePath.string());
    prefs.SetJson(kRecentScenesPrefsKey, recentArray);
    (void)prefs.Save(&err);
}

void SceneEditorController::PollAutoSavePreferences()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.TryGetBool("editor.autoSaveEnabled", m_AutoSaveEnabledCached);
    double interval = kAutoSaveIntervalDefaultSec;
    if (prefs.TryGetDouble("editor.autoSaveIntervalSeconds", interval))
    {
        interval = std::clamp(interval, kAutoSaveIntervalMinSec, kAutoSaveIntervalMaxSec);
    }
    m_AutoSaveIntervalCachedSec = static_cast<float>(interval);
}

void SceneEditorController::TickAutoSave(float deltaSeconds, bool inhibitWhileNotEditing)
{
    if (!m_Doc || inhibitWhileNotEditing)
    {
        m_AutoSaveAccumSeconds = 0.0f;
        return;
    }

    // Don't back up a half-resolved world: a deferred scene build can outlast the
    // autosave interval, and a backup taken mid-pump would persist a scene whose
    // entities aren't fully resolved yet. Hold the timer until the build finishes.
    if (m_Doc->IsSceneBuildInProgress())
    {
        m_AutoSaveAccumSeconds = 0.0f;
        return;
    }

    m_AutoSavePrefsPollAccum += deltaSeconds;
    if (m_AutoSavePrefsPollAccum >= kAutoSavePrefsPollSeconds)
    {
        m_AutoSavePrefsPollAccum = 0.0f;
        PollAutoSavePreferences();
    }

    if (!m_AutoSaveEnabledCached || !m_Doc->HasScenePath())
    {
        m_AutoSaveAccumSeconds = 0.0f;
        return;
    }

    m_AutoSaveAccumSeconds += deltaSeconds;
    if (m_AutoSaveAccumSeconds < m_AutoSaveIntervalCachedSec)
        return;

    m_AutoSaveAccumSeconds = 0.0f;

    if (m_Doc->SaveBackupCopy())
    {
        Logger::Log::Debug("SceneEditor: auto-saved backup scene");
    }
}

} // namespace GameEngine::Editor

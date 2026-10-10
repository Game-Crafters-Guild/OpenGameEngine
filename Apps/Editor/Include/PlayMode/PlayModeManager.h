#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace MarkupECS
{
struct MarkupWorldNotes;
}
namespace ECS
{
class World;
}

namespace Editor
{
class EditorChangeNotifications;
class SceneEditorController;
class UndoRedoService;
class IEditorCommand;

enum class PlayModeState : std::uint8_t
{
    Edit = 0,
    EnteringPlay,
    Play,
    Paused,
    ExitingPlay,
    ChangeReview
};

// Minimal Play Mode orchestrator (V1):
// - same-world snapshot/restore via SceneEditorController/SceneDocumentManager
// - system gating hooks (to be expanded)
// - pause state tracking
// - change review staging hook (UI to be added)
class PlayModeManager
{
  public:
    using Snapshot = std::vector<std::uint8_t>;
    struct PendingChange
    {
        std::unique_ptr<class IEditorCommand> cmd;
        bool selected = true;
    };

    PlayModeManager();
    ~PlayModeManager();

    PlayModeManager(const PlayModeManager&) = delete;
    PlayModeManager& operator=(const PlayModeManager&) = delete;

    void SetWorld(ECS::World* world);
    void SetSceneEditor(SceneEditorController* sceneEditor);
    void SetUndoRedo(UndoRedoService* undoRedo);
    void SetChangeNotifications(EditorChangeNotifications* notifications);

    PlayModeState GetState() const { return m_State; }
    bool IsPlayingOrPaused() const { return m_State == PlayModeState::Play || m_State == PlayModeState::Paused; }
    bool IsPlaying() const { return m_State == PlayModeState::Play; }
    bool IsPaused() const { return m_State == PlayModeState::Paused; }

    // UI entrypoints
    void TogglePlayStop(); // Enter if in Edit, otherwise Exit
    void TogglePause();    // Only valid while playing/paused

    // Explicit entrypoints
    void EnterPlayMode();
    void ExitPlayMode();

    // Change review (valid when state == ChangeReview)
    bool HasPendingChanges() const;
    std::vector<std::string> GetPendingChangeNames() const;
    void ApplyPendingChanges(const std::vector<bool>& keep);
    void DiscardPendingChanges();

    // Returns true if the entity (by packed id) was created during play mode
    // (i.e., not present in the pre-play snapshot). Only meaningful while playing/paused.
    bool IsRuntimeEntity(std::uint32_t packedEntityId) const;

    // Per-frame tick while playing (best-effort; no-op while paused/editing).
    void Tick(float deltaSeconds);

    void SetOnStateChanged(std::function<void(PlayModeState)> cb) { m_OnStateChanged = std::move(cb); }

    // Invalidate cached managed Play Mode export pointers immediately.
    // Intended to be called when the managed runtime domain is swapped/unloaded.
    void InvalidateManagedPlayModeBindings();

    // Apply default system gating for Edit mode (runtime simulation OFF).
    // Intended to be called after the engine rendering loop is enabled.
    void ApplyEditModeSystemGating();

  private:
    struct ManagedPlayModeCallbacks
    {
        void* enter = nullptr;
        void* exit = nullptr;
        void* tick = nullptr;
        bool triedResolve = false;

        bool IsValid() const { return enter != nullptr && exit != nullptr && tick != nullptr; }
        void Clear()
        {
            enter = nullptr;
            exit = nullptr;
            tick = nullptr;
        }
    };

    struct UndoHistoryStash;

    void SetState(PlayModeState s);

    Snapshot CaptureSnapshot() const;
    bool ApplySnapshot(const Snapshot& snapshot) const;

    // Managed PlayMode hooks (Editor.Managed) are resolved lazily and cached.
    // This binds native-callable exports via CoreCLRHost and is safe to no-op if missing.
    void EnsureManagedPlayModeBindings();

  private:
    PlayModeState m_State = PlayModeState::Edit;
    ECS::World* m_World = nullptr; // not owned
    SceneEditorController* m_SceneEditor = nullptr; // not owned
    UndoRedoService* m_UndoRedo = nullptr; // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned

    std::optional<Snapshot> m_EnteredSnapshot;
    std::unordered_set<std::uint32_t> m_PrePlayEntityIds; // packed entity ids present before play
    std::unique_ptr<UndoHistoryStash> m_UndoStash;
    // The mark-ups' notes as play began: the snapshot restore resets the world, which drops
    // them from MarkupService, and brings the same entities back under their handles.
    std::unique_ptr<MarkupECS::MarkupWorldNotes> m_EnteredMarkupNotes;
    std::vector<PendingChange> m_PendingChanges;
    ManagedPlayModeCallbacks m_ManagedPlayMode;

    std::function<void(PlayModeState)> m_OnStateChanged;
};

} // namespace Editor
} // namespace GameEngine


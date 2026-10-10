#include "PlayMode/PlayModeManager.h"
#include "PlayMode/ModelAnimationPlayMode.h"
#include "Inspectors/AnimationPreviewManager.h"
#include "EditorChangeNotifications.h"

#include "Scene/SceneEditorController.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Audio/AudioSystem.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "NativeScripting/NativeScriptManager.h" // SetHotReloadDeferred during play
#include "NativeScripting/UserSystemRegistry.h"  // user C++ system play-mode tick
#include "Engine/GameUI/GameplayUI.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "TerrainECS/TerrainService.h"
#include "MarkupECS/MarkupService.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptManager.h"

namespace GameEngine::Editor
{
static void SetRuntimeSimulationSystemsEnabled(bool enabled)
{
    // PhysicsECS systems (names come from PhysicsECS::AddPhysicsSystemsToSchedule).
    EngineCore::GetInstance().SetRenderingSystemEnabled("PhysicsWorldBootstrap", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("PhysicsEvents", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("PhysicsInit", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("CharacterController", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("PhysicsStep", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("CharacterControllerWriteback", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("PhysicsWriteback", enabled);

    // Rendering-loop animation system name is "Animation" (see RegisterRenderingSystems.cpp).
    // Don't unconditionally disable Animation: AnimationPreviewManager may have it
    // enabled for an in-editor preview that must survive play-mode exit. The system
    // is otherwise owned by the preview manager via UpdateAnimationSystemEnabled().
    const bool keepAnimationForPreview = !enabled && AnimationPreviewManager::AnyPreviewActive();
    EngineCore::GetInstance().SetRenderingSystemEnabled(
        "Animation", enabled || keepAnimationForPreview);

    // Audio ECS systems (names come from Engine::Audio::AddAudioSystemsToSchedule).
    EngineCore::GetInstance().SetRenderingSystemEnabled("AudioListener", enabled);
    EngineCore::GetInstance().SetRenderingSystemEnabled("AudioEmitter", enabled);
}

static void ResetTransientRenderState(ECS::World& world)
{
    // Every cached GPU slot has to be dropped on a play-mode boundary,
    // including the ones belonging to entities that are switched off.
    world.Query<ECS::Write<Components::MeshGPUData>>()
        .IncludeDisabled()
        .Each([](Components::MeshGPUData& meshGpu)
              {
                  meshGpu = Components::MeshGPUData{};
              });
}

struct PlayModeManager::UndoHistoryStash
{
    UndoRedoService::History editHistory{};
    UndoRedoService::History playHistory{};
};

PlayModeManager::PlayModeManager() = default;
PlayModeManager::~PlayModeManager() = default;

void PlayModeManager::InvalidateManagedPlayModeBindings()
{
    m_ManagedPlayMode.Clear();
    m_ManagedPlayMode.triedResolve = false;
}

void PlayModeManager::EnsureManagedPlayModeBindings()
{
    if (m_ManagedPlayMode.IsValid())
        return;
    if (m_ManagedPlayMode.triedResolve)
        return;

    m_ManagedPlayMode.Clear();
    m_ManagedPlayMode.triedResolve = true;

    const std::filesystem::path dllPath =
        ScriptingPaths::ResolveEngineManagedDirectory() / "GameEngine.Editor.Managed.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dllPath, ec))
    {
        Logger::Log::Warning("PlayMode: Editor managed assembly not found at '{}'", dllPath.string());
        return;
    }

    auto& clrHost = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
    const std::string typeName = "GameEngine.Editor.Managed.PlayModeDriverExports, GameEngine.Editor.Managed";
    void* enterPtr = clrHost.GetManagedFunction(dllPath.string(), typeName, "OnEnter");
    void* exitPtr = clrHost.GetManagedFunction(dllPath.string(), typeName, "OnExit");
    void* tickPtr = clrHost.GetManagedFunction(dllPath.string(), typeName, "OnTick");
    if (!enterPtr || !exitPtr || !tickPtr)
        return;

    m_ManagedPlayMode.enter = enterPtr;
    m_ManagedPlayMode.exit = exitPtr;
    m_ManagedPlayMode.tick = tickPtr;
}

bool PlayModeManager::IsRuntimeEntity(std::uint32_t packedEntityId) const
{
    if (!IsPlayingOrPaused())
        return false;
    if (m_PrePlayEntityIds.empty())
        return false;
    return m_PrePlayEntityIds.find(packedEntityId) == m_PrePlayEntityIds.end();
}

void PlayModeManager::SetWorld(ECS::World* world)
{
    m_World = world;
}

void PlayModeManager::SetSceneEditor(SceneEditorController* sceneEditor)
{
    m_SceneEditor = sceneEditor;
}

void PlayModeManager::SetUndoRedo(UndoRedoService* undoRedo)
{
    m_UndoRedo = undoRedo;
}

void PlayModeManager::SetChangeNotifications(EditorChangeNotifications* notifications)
{
    m_Notifications = notifications;
}

void PlayModeManager::ApplyEditModeSystemGating()
{
    if (m_State != PlayModeState::Edit)
        return;
    SetRuntimeSimulationSystemsEnabled(false);
}

void PlayModeManager::TogglePlayStop()
{
    if (m_State == PlayModeState::Edit)
    {
        EnterPlayMode();
    }
    else if (m_State == PlayModeState::ChangeReview)
    {
        DiscardPendingChanges();
    }
    else
    {
        ExitPlayMode();
    }
}

void PlayModeManager::TogglePause()
{
    if (m_State == PlayModeState::Play)
    {
        // Pause runtime simulation systems (rendering continues).
        SetRuntimeSimulationSystemsEnabled(false);
        SetState(PlayModeState::Paused);
        return;
    }
    if (m_State == PlayModeState::Paused)
    {
        SetRuntimeSimulationSystemsEnabled(true);
        SetState(PlayModeState::Play);
        return;
    }
}

void PlayModeManager::EnterPlayMode()
{
    if (m_State != PlayModeState::Edit)
        return;

    if (!m_World)
    {
        Logger::Log::Warning("PlayMode: cannot enter (no world)");
        return;
    }

    // A deferred scene build is still resolving entities across frames. Entering
    // now would snapshot a half-built world (some meshes/skeletons unresolved) and
    // the pump would keep mutating it under play mode. Refuse until it finishes.
    if (m_SceneEditor && m_SceneEditor->IsSceneBuildInProgress())
    {
        Logger::Log::Warning("PlayMode: cannot enter while the scene is still loading");
        return;
    }

    SetState(PlayModeState::EnteringPlay);

    // Isolate undo history for play mode.
    if (m_UndoRedo)
    {
        m_UndoStash = std::make_unique<UndoHistoryStash>();
        m_UndoStash->editHistory = m_UndoRedo->DetachHistory();
        // Start with a clean play-mode history (DetachHistory already cleared stacks).
        m_UndoStash->playHistory = {};
    }

    // Snapshot brush-authored terrain zone payloads: edits made during Play
    // restore on Play-exit by default (edit-pipeline design §7/§10), matching
    // component-state semantics. The ECS snapshot doesn't cover them — the
    // payload data lives in the TerrainService store, not on components.
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
        terrain->SnapshotZonePayloadsForPlay();

    // The mark-ups' notes live in MarkupService, not on components, so the ECS snapshot
    // does not carry them either; the exit restore puts them back.
    if (auto* markups = MarkupECS::MarkupService::TryGet(); markups && m_World)
        m_EnteredMarkupNotes = std::make_unique<MarkupECS::MarkupWorldNotes>(markups->CaptureWorld(*m_World));

    // V1 world isolation: same world snapshot/restore.
    m_EnteredSnapshot = CaptureSnapshot();
    if (!m_EnteredSnapshot.has_value() || m_EnteredSnapshot->empty())
    {
        Logger::Log::Warning("PlayMode: failed to capture world snapshot; staying in Edit");
        m_EnteredSnapshot.reset();
        m_EnteredMarkupNotes.reset();
        // Restore undo history if we detached it.
        if (m_UndoRedo && m_UndoStash)
        {
            m_UndoRedo->AttachHistory(std::move(m_UndoStash->editHistory));
            m_UndoStash.reset();
        }
        SetState(PlayModeState::Edit);
        return;
    }

    // Record which entities exist before play so the hierarchy can distinguish runtime-created ones.
    {
        std::vector<ECS::EntityHandle> alive;
        m_World->GetAliveEntitiesSnapshot(alive);
        m_PrePlayEntityIds.clear();
        m_PrePlayEntityIds.reserve(alive.size());
        for (const auto& e : alive)
            m_PrePlayEntityIds.insert(e.id);
    }

    // Hot-reload is allowed during play mode: the assembly swap executes on the
    // main thread between frames, and PlayModeDriver detects the domain change
    // to rebuild hooks and re-register entity/game systems automatically.

    // Clear any active animation previews before play mode takes over.
    Editor::AnimationPreviewManager::DisableAllPreviews(*m_World);

    // TODO(p4_runtime_gating): enable runtime systems (physics step, animation, user tick, etc).
    SetRuntimeSimulationSystemsEnabled(true);

    ApplyAnimatorOnEnterPlayMode(*m_World);

    // Defer native-script rebuilds while playing: an edit during play queues until Stop instead
    // of reloading mid-frame, which would ClearUserSystems + re-register without firing
    // OnDestroy/OnStart (stranding the lifecycle). Flushed in ExitPlayMode.
    if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
        nativeScripts->SetHotReloadDeferred(true);

    // User C++ systems begin: OnStart on every registered system. They tick only during play,
    // after the snapshot — so any entities OnStart spawns are cleaned up by the exit restore.
    GameEngine::NativeScripting::StartUserSystems(*m_World);

    // Managed play-mode enter hook (native binds cached function pointers). A
    // failed resolve must NOT latch for the whole session: on a fresh-cache cold
    // start the first attempt can run before the CLR/scripts finished coming up,
    // and the per-frame Tick latch (triedResolve) would otherwise keep managed
    // play mode dead until an editor restart. Re-attempt on every play-enter.
    if (!m_ManagedPlayMode.IsValid())
        m_ManagedPlayMode.triedResolve = false;
    EnsureManagedPlayModeBindings();
    if (!m_ManagedPlayMode.IsValid())
    {
        Logger::Log::Error(
            "PlayMode: managed play-mode bindings unavailable — C# GameSystems will NOT run "
            "this Play session (will re-attempt on the next play-enter; check earlier "
            "script-compile/CLR errors)");
    }

    if (m_ManagedPlayMode.enter)
    {
        using EnterFn = int32_t(CORECLR_DELEGATE_CALLTYPE*)();
        auto fn = reinterpret_cast<EnterFn>(m_ManagedPlayMode.enter);
        const int32_t rc = fn();
        if (rc != 0)
        {
            Logger::Log::Warning("PlayMode: managed OnEnter returned rc={}", rc);
        }

        // Managed play-mode hooks may have created/destroyed entities.
        // Notify the editor so the hierarchy panel refreshes.
        if (m_Notifications && m_World)
        {
            EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = m_World;
            m_Notifications->NotifyWorldStructureChanged(e);
        }
    }

    SetState(PlayModeState::Play);
}

void PlayModeManager::ExitPlayMode()
{
    if (m_State == PlayModeState::Edit)
        return;
    if (m_State == PlayModeState::ChangeReview)
        return;

    SetState(PlayModeState::ExitingPlay);

    // Best-effort managed play-mode exit hook (called before tearing down runtime systems).
    EnsureManagedPlayModeBindings();

    if (m_ManagedPlayMode.exit)
    {
        using ExitFn = int32_t(CORECLR_DELEGATE_CALLTYPE*)();
        auto fn = reinterpret_cast<ExitFn>(m_ManagedPlayMode.exit);
        const int32_t rc = fn();
        if (rc != 0)
        {
            Logger::Log::Warning("PlayMode: managed OnExit returned rc={}", rc);
        }
    }

    // Revert any terrain zone payloads edited during Play (design §7/§10).
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
        terrain->RestoreZonePayloadsFromPlaySnapshot();

    // User C++ systems end: OnDestroy on each (before the snapshot restore, so a system can tear
    // down resources it created while the play world is still live).
    if (m_World)
        GameEngine::NativeScripting::StopUserSystems(*m_World);

    // Drop the play HUD subtrees (and bump the gameplay session). The next
    // Game View SyncFromWorld rebuilds from the restored UIDocument.
    ::GameEngine::GameUI::NotifyGameplayStopped();

    // Disable runtime simulation systems first (rendering continues).
    SetRuntimeSimulationSystemsEnabled(false);

    // Hard-reset the singleton physics world when leaving play.
    // Otherwise, PhysicsECS can retain bodies/contacts across sessions even after the ECS snapshot is restored,
    // which shows up as "invisible colliders" or stale interactions on the next Play.
    if (GameEngine::PhysicsECS::PhysicsWorldService::IsInitialized())
    {
        GameEngine::PhysicsECS::PhysicsWorldService::Shutdown();
    }

    // Release per-tile and per-face terrain-collider slots. The collider entities
    // are runtime-only and vanish with the snapshot restore below, but that path
    // never runs the TerrainPhysicsSystem teardown sweep, so their tile-physics /
    // planet-face slots would otherwise leak across Play sessions.
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
    {
        terrain->ReleaseAllTilePhysicsHandles();
        terrain->ReleaseAllPlanetFacePhysicsHandles();
    }

    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
    {
        audio->StopAllVoices();
    }

    if (m_EnteredSnapshot.has_value())
    {
        (void)ApplySnapshot(*m_EnteredSnapshot);
        if (m_World)
            ResetTransientRenderState(*m_World);
        if (auto* markups = MarkupECS::MarkupService::TryGet(); markups && m_World && m_EnteredMarkupNotes)
            markups->RestoreWorld(*m_World, std::move(*m_EnteredMarkupNotes));
    }
    m_EnteredSnapshot.reset();
    m_EnteredMarkupNotes.reset();
    m_PrePlayEntityIds.clear();

    // Re-enable native hot-reload and flush any source edit made during play (deferred in
    // EnterPlayMode) now that we're back in edit mode — so the rebuild + re-register happens
    // cleanly outside the play loop.
    if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
    {
        nativeScripts->SetHotReloadDeferred(false);
        nativeScripts->TriggerDeferredHotReloadIfPending();
    }

    // Snapshot restore may have created/destroyed entities.
    // Notify the editor so the hierarchy panel refreshes.
    if (m_Notifications && m_World)
    {
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

    // Capture play-mode undo stack for future change review, then restore edit-mode history.
    if (m_UndoRedo && m_UndoStash)
    {
        m_UndoStash->playHistory = m_UndoRedo->DetachHistory();
        m_UndoRedo->AttachHistory(std::move(m_UndoStash->editHistory));

        // Convert play undo history into pending changes (in chronological order).
        m_PendingChanges.clear();
        m_PendingChanges.reserve(m_UndoStash->playHistory.undo.size());
        for (auto& cmd : m_UndoStash->playHistory.undo)
        {
            if (cmd)
            {
                PendingChange c{};
                c.cmd = std::move(cmd);
                c.selected = true;
                m_PendingChanges.push_back(std::move(c));
            }
        }

        // Discard redo stack and release stash storage (we moved commands out).
        m_UndoStash.reset();
    }

    if (!m_PendingChanges.empty())
    {
        SetState(PlayModeState::ChangeReview);
    }
    else
    {
        SetState(PlayModeState::Edit);
    }

    // Reset cached pointers after play mode ends; next entry will re-resolve.
    InvalidateManagedPlayModeBindings();
}

bool PlayModeManager::HasPendingChanges() const
{
    return !m_PendingChanges.empty();
}

std::vector<std::string> PlayModeManager::GetPendingChangeNames() const
{
    std::vector<std::string> names;
    names.reserve(m_PendingChanges.size());
    for (const auto& c : m_PendingChanges)
    {
        const char* n = (c.cmd) ? c.cmd->GetName() : nullptr;
        names.emplace_back(n ? n : "Change");
    }
    return names;
}

void PlayModeManager::ApplyPendingChanges(const std::vector<bool>& keep)
{
    if (m_State != PlayModeState::ChangeReview)
        return;
    if (!m_UndoRedo)
    {
        m_PendingChanges.clear();
        SetState(PlayModeState::Edit);
        return;
    }

    const std::size_t n = m_PendingChanges.size();
    const bool keepAll = keep.empty();

    m_UndoRedo->BeginCompound("Apply Play Mode Changes");
    for (std::size_t i = 0; i < n; ++i)
    {
        const bool selected = keepAll ? true : (i < keep.size() ? keep[i] : false);
        if (!selected)
            continue;
        if (m_PendingChanges[i].cmd)
        {
            m_UndoRedo->Execute(std::move(m_PendingChanges[i].cmd));
        }
    }
    m_UndoRedo->EndCompound();

    m_PendingChanges.clear();
    SetState(PlayModeState::Edit);
}

void PlayModeManager::DiscardPendingChanges()
{
    if (m_State != PlayModeState::ChangeReview && m_State != PlayModeState::Edit)
    {
        // Only allow discarding while in review (or as a no-op in edit).
    }
    m_PendingChanges.clear();
    if (m_State == PlayModeState::ChangeReview)
        SetState(PlayModeState::Edit);
}

void PlayModeManager::Tick(float deltaSeconds)
{
    if (m_State != PlayModeState::Play)
        return;

    // No per-frame domain polling here: the export pointers target
    // GameEngine.Editor.Managed, which loads outside the collectible user-scripts
    // ALC and never unloads, and the managed PlayModeDriver re-detects domain
    // swaps itself on each tick (PlayModeDriver.EnsureBuilt rebuilds its hooks
    // when HotReloadManager's domain id changes). Explicit domain unloads still
    // invalidate the cached pointers via the SetOnDomainWillUnload hook wired in
    // EditorApplication, and EnsureManagedPlayModeBindings re-resolves lazily.
    EnsureManagedPlayModeBindings();

    if (m_ManagedPlayMode.tick)
    {
        using TickFn = int32_t(CORECLR_DELEGATE_CALLTYPE*)(float);
        auto fn = reinterpret_cast<TickFn>(m_ManagedPlayMode.tick);
        const int32_t rc = fn(deltaSeconds);
        if (rc != 0)
        {
            Logger::Log::Warning("PlayMode: managed OnTick returned rc={}", rc);
        }
    }

    // User C++ systems: OnUpdate(world, dt) on each, every play frame.
    if (m_World)
        GameEngine::NativeScripting::TickUserSystems(*m_World, deltaSeconds);
}

void PlayModeManager::SetState(PlayModeState s)
{
    if (m_State == s)
        return;
    m_State = s;
    if (m_OnStateChanged)
        m_OnStateChanged(m_State);
}

PlayModeManager::Snapshot PlayModeManager::CaptureSnapshot() const
{
    if (m_SceneEditor)
    {
        // Prefer editor document snapshot (keeps doc manager state consistent).
        auto snap = m_SceneEditor->CaptureWorldSnapshot();
        if (!snap.empty())
            return snap;
    }

    if (!m_World)
        return {};
    return m_World->SerializeWorld();
}

bool PlayModeManager::ApplySnapshot(const Snapshot& snapshot) const
{
    if (m_SceneEditor)
    {
        if (m_SceneEditor->ApplyWorldSnapshot(snapshot))
            return true;
    }

    if (!m_World)
        return false;
    m_World->DeserializeWorld(snapshot);
    m_World->ProcessCommands();
    return true;
}

} // namespace GameEngine::Editor

#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/Controls/TreeView.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Selection.h"
#include "UI/Interaction/Types.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECS.h"
#include "Editor/Hierarchy/HierarchyRowActivity.h"
#include "EditorChangeNotifications.h"
#include "Scene/SceneIO.h"
#include "VersionControl/SceneDiff.h"

#include <filesystem>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine {

struct Bookmark;
struct EditorContext;
class INativeContextMenu;
class TextField;
class UIElement;
namespace Platform { class Window; }
namespace Editor { class HierarchyNavigationBar; class HierarchyVcsController; class UndoRedoService; }

enum class HierarchySortMode : uint8_t {
    Custom = 0,       // HierarchyOrder component (scene-authored order)
    Alphabetical = 1, // by Name, case-insensitive
    Type = 2,         // by primary component category, name as tie-break
};

enum class HierarchySortDirection : uint8_t {
    Ascending = 0,
    Descending = 1,
};

class HierarchyPanel : public DockPanel, public UI::Interaction::IDropTarget {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-hierarchy-icon"; }

    HierarchyPanel();
    ~HierarchyPanel() override;

    void SetContext(const EditorContext* ctx);

    // Optional: target world (defaults to EngineCore::GetInstance().EnsurePrimaryWorld())
    void SetWorld(ECS::World* world);

    /// Supplies the active saved scene's provider-neutral VCS diff. Entity
    /// rows use it to display the same Added/Modified/Removed dots as assets
    /// and Inspector properties.
    void SetSceneDiffProvider(std::function<std::vector<Editor::SceneObjectDiff>()> provider);

    /// One baseline-only row to show alongside the live entities. Declared here
    /// because the data provider is an implementation detail of this panel.
    struct GhostRow
    {
        TreeId Id = 0;
        TreeId ParentId = 0;
        std::string Label;
    };
    void SetGhostRows(std::vector<GhostRow> rows);
    /// Row id for a live entity, so callers can parent a ghost under one.
    static TreeId EntityTreeId(const ECS::EntityHandle& entity);
    void SetScenePathProvider(std::function<std::optional<std::filesystem::path>()> provider);
    // Lets Hierarchy Options offer the scene-diff indicator toggle. The action
    // is the host's because the preference also drives the Inspector.
    void SetSceneDiffIndicatorToggle(std::function<void()> toggle);
    void RefreshSceneDiffDecorations();

    /// Supplies embedded [hierarchy_ui] snapshot from the last scene load (consumed once per refresh).
    void SetPendingHierarchyUiProvider(std::function<std::optional<Scene::SceneHierarchyUiFromFile>()> fn)
    {
        m_PendingHierarchyUiProvider = std::move(fn);
    }

    Scene::SceneHierarchyUi CaptureHierarchyUiForSceneSave();

    void Refresh(bool restoreHierarchyUiFromSceneAfterCommit = false);

    /// Per-frame poll (wired via EditorPanelManager::RegisterUpdateCallback). Catches world
    /// structural mutations that arrive from paths which don't emit EditorChangeNotifications —
    /// e.g. the debug-server IPC bulk spawn, C# scripts, gameplay systems. Compares the World's
    /// structural-change version against the last-observed value and rebuilds only when it moved,
    /// so an idle hierarchy costs a single atomic load per frame.
    void Update();

    // Deterministic command invocation (used by UI replay automation).
    // Handles built-in Hierarchy commands (e.g. create entity primitives).
    // Returns true if the command was handled.
    bool HandleCommand(std::uint32_t cmd);

    // UI tweak: control the TreeView row height (pixels).
    void SetTreeRowHeight(float px);
    float GetTreeRowHeight() const;
    // UI tweak: control the TreeView child indentation (pixels).
    void SetTreeChildIndent(float px);
    // UI tweak: control the TreeView icon size (pixels).
    void SetTreeIconSize(float px);

    /** A tree icon size to commit, from the item resize gesture over the tree or from the size slider; editor wires to Settings + prefs. */
    void SetOnTreeIconSizeWheelCommit(std::function<void(float)> cb) { m_OnTreeIconSizeWheelCommit = std::move(cb); }

    // Selection callback (Editor wires this to Inspector later)
    void SetOnSelectEntity(std::function<void(ECS::EntityHandle)> cb) { m_OnSelectEntity = std::move(cb); }
    void SetOnSelectEntities(std::function<void(const std::vector<ECS::EntityHandle>&)> cb) { m_OnSelectEntities = std::move(cb); }

    // Hover callback (Editor wires this to scene view hover preview)
    void SetOnHoverEntity(std::function<void(ECS::EntityHandle)> cb) { m_OnHoverEntity = std::move(cb); }
    
    // Select an entity by handle
    void SelectEntity(ECS::EntityHandle entity);

    /// Select multiple entities (e.g. after multi-asset Scene View drop). Anchor is the first handle in the list.
    void SelectEntities(const std::vector<ECS::EntityHandle>& entities);

    /// Apply selection without recording a separate "Hierarchy Selection" undo step (caller owns undo).
    void ApplySelectionProgrammatic(const std::vector<UI::Interaction::ItemId>& ids, UI::Interaction::ItemId anchor);

    /// Scene View picking updates the inspector via SceneViewController; mirror that selection into the
    /// hierarchy SelectionModel so "Hierarchy Selection" undo entries are created (tree clicks alone do not
    /// run when the viewport initiates selection).
    void SyncSelectionWithSceneViewPick(ECS::EntityHandle entity);

    /// Remember hierarchy selection before play mode mutates/restores the world; restored after refresh.
    void StashSelectionForPlayModeTransition();
    void RestoreStashedSelectionAfterPlayModeTransition();

    std::vector<UI::Interaction::ItemId> GetSelectionItemIds() const;
    UI::Interaction::ItemId GetSelectionAnchor() const;

    // When a bookmark is dropped on the hierarchy, call this to navigate (e.g. open scene, select entity).
    void SetOnNavigateToBookmark(std::function<void(const Bookmark&)> cb) { m_OnNavigateToBookmark = std::move(cb); }

    // Called when user adds selected entities to bookmarks (e.g. via context menu).
    void SetOnAddEntitiesToBookmarks(std::function<void(const std::vector<ECS::EntityHandle>&)> cb) { m_OnAddEntitiesToBookmarks = std::move(cb); }

    // Entity created callback (Editor wires this to frame the new entity)
    void SetOnEntityCreated(std::function<void(ECS::EntityHandle)> cb) { m_OnEntityCreated = std::move(cb); }

    // Activation callback: fired when user double-clicks a hierarchy item (e.g. frame in scene view)
    void SetOnHierarchyItemActivated(std::function<void(ECS::EntityHandle)> cb) { m_OnHierarchyItemActivated = std::move(cb); }

    // New Scene callback: fired from context menu "New Scene" action.
    void SetOnNewScene(std::function<void()> cb) { m_OnNewScene = std::move(cb); }
    void SetRecentScenesProvider(std::function<std::vector<std::filesystem::path>()> cb) { m_GetRecentScenes = std::move(cb); }
    void SetOnOpenRecentScene(std::function<void(const std::filesystem::path&)> cb) { m_OnOpenRecentScene = std::move(cb); }

    // Ping asset callback: asks the Assets panel to navigate to and select
    // the given asset (asset-relative path). Used by the hierarchy context
    // menu's "Show Mesh Location" action.
    void SetPingAsset(std::function<void(const std::filesystem::path&)> cb) { m_PingAsset = std::move(cb); }

    // Optional editor undo service (not owned).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    // Optional editor notifications (not owned). Used to refresh the hierarchy on structural changes.
    void SetChangeNotifications(Editor::EditorChangeNotifications* notifications);

    void ApplySearchFilter(const std::string& searchText);
    void ClearSearchHighlights();

    // IDropTarget: accept bookmark drops anywhere on the panel (fallback when not over tree).
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

private:
    // Creates the entity for a dropped asset file once the asset is loaded (a lens flare
    // needs only its GUID): a model named `entityName`, a texture's sprite or a lens flare,
    // placed last in the hierarchy and selected.
    void FinishFileDrop(const std::filesystem::path& assetPath, const GUID& assetGuid,
                        const std::string& entityName);
    // FinishFileDrop once the asset's load lands, never waiting for it; the Scene View says
    // what is loading meanwhile. Dropped when the panel goes, or when a scene opened meanwhile
    // cleared the panel's world (RunWhenAssetLoaded's world check).
    void DeferFileDrop(const std::filesystem::path& assetPath, const GUID& assetGuid,
                       const std::string& entityName);
    // Creates the sprite for a texture dropped on the tree, under `parent` when it is still
    // an entity, and selects it. Returns false when no sprite was made.
    bool FinishTreeTextureDrop(const std::filesystem::path& texturePath, ECS::EntityHandle parent);
    // FinishTreeTextureDrop once the texture's load lands, never waiting for it; dropped when
    // the panel goes or a scene opened meanwhile cleared the panel's world. A failed load makes
    // no sprite (the sprite factory reads only a loaded texture).
    void DeferTreeTextureDrop(const std::filesystem::path& texturePath, const GUID& textureGuid,
                              ECS::EntityHandle parent);
    friend class Editor::HierarchyVcsController;
    // Private provider implementation
    class HierarchyDataProvider;

    // Expand every ancestor of `entity` so its row exists in the flattened
    // tree (callers must RefreshFromProvider afterwards).
    void ExpandAncestors(ECS::EntityHandle entity);

    void ApplyTreeIconSizeFromScroll(float scrollY);

    // Drop onto the tree: bookmarks, asset paths, online assets and entity
    // reparenting, parented or ordered by where the drop landed.
    void HandleTreeDrop(const UI::Interaction::DropRequest& req);

    std::unique_ptr<HierarchyDataProvider> m_Provider; // owns provider
    std::unique_ptr<UI::Interaction::SelectionModel> m_Selection; // owns selection model
    TreeView* m_Tree = nullptr; // not owned
    float m_LastTreeIconSizePx = 20.0f;
    std::function<void(float)> m_OnTreeIconSizeWheelCommit;
    Editor::HierarchyNavigationBar* m_NavigationBar = nullptr; // not owned
    UIElement* m_SearchBar = nullptr; // not owned
    TextField* m_SearchField = nullptr; // not owned
    std::string m_SearchFieldScope{"all"};
    ECS::World* m_World = nullptr; // not owned
    std::function<void(ECS::EntityHandle)> m_OnSelectEntity;
    std::function<void(const std::vector<ECS::EntityHandle>&)> m_OnSelectEntities;
    std::function<void(ECS::EntityHandle)> m_OnHoverEntity;
    std::function<void(const Bookmark&)> m_OnNavigateToBookmark;
    std::function<void(const std::vector<ECS::EntityHandle>&)> m_OnAddEntitiesToBookmarks;
    std::function<void(ECS::EntityHandle)> m_OnEntityCreated;
    std::function<void(ECS::EntityHandle)> m_OnHierarchyItemActivated;
    std::function<void()> m_OnNewScene;
    std::function<std::vector<std::filesystem::path>()> m_GetRecentScenes;
    std::function<void(const std::filesystem::path&)> m_OnOpenRecentScene;
    std::function<void(const std::filesystem::path&)> m_PingAsset;

    const EditorContext* m_Context = nullptr; // not owned
    Platform::Window* m_Window = nullptr; // not owned
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
    std::unique_ptr<Editor::HierarchyVcsController> m_VcsController;

    Editor::UndoRedoService* m_Undo = nullptr; // not owned
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr; // not owned
    Editor::EditorChangeNotifications::SubscriptionToken m_StructureSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_LightChangedSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_MeshRendererChangedSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_SkinnedMeshRendererChangedSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_RenderLayerChangedSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_NameChangedSub{};
    bool m_RefreshQueued = false;
    // Coalesces the Name-changed label rebuild, which deliberately bypasses m_RefreshQueued
    // (a queued label rebuild must never swallow a pending structure refresh). Update()'s
    // value-change scan checks both flags so it never schedules a redundant rebuild.
    bool m_NameRebuildQueued = false;

    // Last World structural-change version the provider was rebuilt against. Owned by Update():
    // seeded in SetWorld and advanced whenever a poll observes movement, so event-driven and
    // poll-driven refreshes never redundantly rebuild each other.
    std::size_t m_LastWorldStructuralVersion = 0;

    // LifecycleEvents (Parent) consumer bookkeeping for the incremental sync path.
    // Reset generation (World::Clear / scene swap): change => full Rebuild fallback.
    // Swap generation (F1 cadence guard): a gap > 1 means >=1 event window was discarded
    // unseen (panel hidden/throttled); since a parent-component REMOVE is not rediscoverable
    // by the identity diff or the Changed<Parent> scan, a gap heals via the full Rebuild
    // fallback (design §2 "gap > 1 => full rebuild"). Both re-seeded on every full rebuild.
    uint64 m_LastLifecycleResetGen = 0;
    uint64 m_LastLifecycleSwapGen = 0;

    // Legacy detect-then-full-rebuild Update path, preserved verbatim for the
    // GE_HIERARCHY_INCREMENTAL=0 kill switch (restores today's behavior exactly).
    void UpdateLegacyFullRebuild();

    // Re-seed the panel-side incremental baselines (lifecycle generations) from the current
    // world. Called wherever a full Rebuild re-establishes provider truth (SetWorld, Refresh).
    void ReseedIncrementalBaselines();

    // Gate for Update()'s Changed<Parent>/Changed<Name> scans. A data-only Set<Parent>/
    // Set<Name> (gameplay reparent, script rename) stamps the chunk column but bumps no
    // structural version and emits no notification, so without this scan the tree keeps
    // showing the old parent/name. One shared gate: both scans feed the same consumer
    // (the provider rebuild) and always advance together (the TransformHierarchySystem
    // m_SerialGate precedent). Every rebuild path re-arms it via ArmValueChangeGate() so
    // notification-driven refreshes don't re-trigger the scan.
    // Standing caveat: a system that write-grants Parent or Name columns EVERY frame
    // (non-const Parent*/Name* in a per-frame query) would turn this detector into a
    // full provider rebuild per frame. Nothing in-tree does; keep it that way.
    ECS::ChangeGate m_ValueChangeGate;

    /** Entry-sample the world's global system version into m_ValueChangeGate. Callers arm
        BEFORE reading world state (ChangeGate contract): a write landing mid-rebuild then
        compares greater on the next Update() and re-triggers a refresh instead of being lost. */
    void ArmValueChangeGate();

    /** Look up the pooled row bound to `handle` and refresh its light tint (no-op if the row isn't currently bound). */
    void RefreshLightTintForEntity(ECS::EntityHandle handle);
    /** Apply `hierarchy-icons-colored` class to this instance's UIManager root and refresh every bound row's tint. */
    void ApplyColoredIconsMode(bool colored);
    /** Apply `hierarchy-model-thumbs-colored` class to this instance's UIManager root and refresh every bound model-thumb row. */
    void ApplyModelThumbsAlwaysColoredMode(bool colored);
    /** If titleEl is a model thumbnail row and the current UIManager root indicates model thumbs should be colored, force saturation=1 and tint=white. */
    static void ApplyModelThumbColorOverride(UIElement* titleEl);
    /** Rebinds, through the tree's provider, every row the tree holds now. */
    void RePresentHeldRows();

public:
    /** Push the "Colored Hierarchy Icons" state to every live HierarchyPanel so all open hierarchies update without needing focus. */
    static void NotifyColoredIconsChanged(bool colored);
    /** Push the "3D Model Thumbnails Always Colored" state to every live HierarchyPanel. */
    static void NotifyModelThumbsAlwaysColoredChanged(bool colored);

    bool IsEntityLocked(ECS::EntityHandle handle) const
    {
        return handle.IsValid() && m_LockedEntityIds.count(handle.id) != 0;
    }

private:

    // Track selection state for undo/redo of hierarchy selection.
    std::vector<UI::Interaction::ItemId> m_LastSelectionIds;
    UI::Interaction::ItemId m_LastSelectionAnchor = 0;
    bool m_SuppressSelectionUndo = false;
    // While syncing hierarchy from SceneViewController; avoids OnEntityPicked → inspector → sync → loop.
    bool m_SkipOnSelectEntity = false;
    // Set during hierarchy-initiated selection to prevent SceneView round-trip from resetting multi-select.
    bool m_SkipSyncFromSceneView = false;

    // The rows' enable state: shown at bind, re-shown through the provider when it moves.
    Editor::HierarchyRowActivity m_RowActivity;

    std::string m_CurrentSearchText;
    std::unordered_set<TreeId> m_SearchMatchIds;
    std::unordered_set<TreeId> m_SearchVisibleIds;
    std::vector<UIElement*> m_SearchHighlightedElements;

    // Current context-menu target (captured at Show-time; used by the native menu command handler).
    ECS::EntityHandle m_ContextTargetEntity{};
    std::vector<std::filesystem::path> m_ContextMenuRecentScenes;

    // Duplicate the currently selected entities (with subtrees) and select the copies.
    void DuplicateSelectedEntities();

    /** Entity IDs currently locked in the hierarchy (editor-only, not serialized). */
    std::unordered_set<std::uint32_t> m_LockedEntityIds;

    std::function<std::optional<Scene::SceneHierarchyUiFromFile>()> m_PendingHierarchyUiProvider;

    // Shared flag so detached background threads can detect panel destruction.
    std::shared_ptr<std::atomic<bool>> m_Alive = std::make_shared<std::atomic<bool>>(true);

    void RestoreHierarchyUiFromSceneAfterSceneCommit();

    struct PlayModeSelectionStash
    {
        std::vector<std::string> SceneTagsOrdered;
        std::vector<ECS::EntityHandle> EntitiesOrdered;
    };

    std::optional<PlayModeSelectionStash> CaptureSelectionForPlayModeTransition() const;
    void ApplySelectionFromPlayModeStash(const PlayModeSelectionStash& stash);
    void RevalidateAndSyncSelection();

    std::optional<PlayModeSelectionStash> m_PlayModeSelectionStash;

    std::vector<ECS::EntityHandle> ResolveRowActionTargets(ECS::EntityHandle clicked) const;

    HierarchySortMode m_SortMode = HierarchySortMode::Custom;
    HierarchySortDirection m_SortDirection = HierarchySortDirection::Ascending;
    bool m_ShowRenderLayer = false;

    void ApplySortSettings();
    void ApplyRenderLayerVisibility();
    void SetHorizontalBarPresent(bool present);

    std::function<void(TreeId, float, float)> m_ShowContextMenu;
};

} // namespace GameEngine

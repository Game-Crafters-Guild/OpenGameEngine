#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include "AssetCore/GUID.h"
#include "Graph/GraphTransitionStore.h"
#include "ECS/ECS.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorEntityActivity.h"
#include "Types/Types.h"
#include "VersionControl/SceneDiff.h"

#include "UI/Controls/DockPanel.h"
#include "Audio/AudioHandles.h"
#include "Rendering/Core/Handle.h"

#include <chrono>

namespace GameEngine {

struct EditorContext;
class INativeContextMenu;
namespace Platform { class Window; }

class Asset;
class AssetFuture;
class UIElement;
class Label;
class ScrollView;
class TextField;
class Dropdown;
class Button;
class InspectorSection;
class Toggle;
struct ScriptVariable;
struct ScriptMethod;
struct SmartFolder;
class SmartFolderManager;
class SmartFolderInspector;
class SearchDialog;
class ComponentPresetSearchProvider;
class ConfirmActionModal;

namespace ECS { class World; struct EntityHandle; }
namespace Editor { class InspectorVcsController; class UndoRedoService; class EditorChangeNotifications; }

struct InspectorSelectionHistoryEntry {
    enum class Kind { Empty, Entity, Entities, Assets, Script, SmartFolder };

    Kind EntryKind = Kind::Empty;
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    std::vector<ECS::EntityHandle> Entities;
    std::vector<std::filesystem::path> AssetPaths;
    std::filesystem::path ScriptPath;
    std::string SmartFolderId;
    SmartFolderManager* SmartFolderMgr = nullptr;

    bool Equals(const InspectorSelectionHistoryEntry& o) const;
};

struct MaterialSlotOrderKey {
    uint32_t EntityPacked = 0;
    GUID ModelGuid{};
    bool operator==(const MaterialSlotOrderKey& o) const
    {
        return EntityPacked == o.EntityPacked && ModelGuid == o.ModelGuid;
    }
};
struct MaterialSlotOrderKeyHash {
    std::size_t operator()(const MaterialSlotOrderKey& k) const;
};

class InspectorPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-inspector-icon"; }

    InspectorPanel();
    ~InspectorPanel() override;

    void SetContext(const EditorContext* ctx);

    // Asset inspection API
    void ShowSelectedAssets(const std::vector<std::filesystem::path>& paths);
    
    // Smart folder inspection API
    void ShowSmartFolder(const std::string& smartFolderId, SmartFolderManager* manager);

    // Entity inspection API.
    // keepMultiSelection: true (default) preserves the multi-selection set when
    // re-showing its anchor (refresh / ShowEntities). A brand-new single selection
    // passes false so the inspector collapses to that one entity instead of keeping
    // a stale multi-set (which would show "mixed" and broadcast edits to old peers).
    void ShowEntity(ECS::World* world, ECS::EntityHandle entity, bool force = false,
                    bool keepMultiSelection = true);
    void ShowEntities(ECS::World* world, const std::vector<ECS::EntityHandle>& entities);
    ECS::EntityHandle GetInspectedEntity() const { return m_Entity; }
    void RefreshCurrentTarget();
    // Refresh a selected target only when a newly loaded/reloaded module owns
    // the custom inspector that should render it. Calls in one load batch are
    // coalesced into one deferred rebuild.
    void RequestRefreshForInspectorModule(std::string_view moduleId);

    // Supplies the active saved scene's VCS diff for in-place entity/property
    // decoration. The callback is evaluated when entity Inspector UI is built.
    void SetSceneDiffProvider(std::function<std::vector<Editor::SceneObjectDiff>()> provider);

    // Re-applies only the scene-diff markers. Cheaper than RefreshCurrentTarget
    // and, unlike it, keeps focus, scroll and in-progress edits intact.
    void RefreshSceneDiffDecorations();

    // Node graph inspection API
    void ShowGraphNode(const std::string& nodeId, const std::string& nodeTypeId, const std::string& displayName,
                       const std::unordered_map<std::string, std::string>& parameters,
                       std::string_view kindId = {});
    void ShowGraphTransition(const std::string& linkId,
                             const std::string& fromName,
                             const std::string& toName,
                             const GraphTransitionDesc& desc);

    // Clears the inspector only when a graph node is what it shows — the
    // GraphPanel deselected it or swapped graphs, so the rows would edit a
    // node the canvas no longer has. Any other content stays untouched.
    void ClearGraphNodeIfShown();

    // Custom tool inspection API
    void ShowCustomInspector(const std::string& title,
                             const std::string& iconClass,
                             std::function<void(UIElement*)> buildContent);

    bool IsLocked() const { return m_Locked; }
    void SetLocked(bool locked);

    /// Call once per frame from the editor main loop. Shows a selected asset whose load
    /// has landed, and refreshes inspector fields from live ECS data during play mode
    /// simulation (throttled internally to ~10 Hz).
    void TickSimulationRefresh();

    /** Re-attach the lock icon to tab:Inspector after a dock rebuild (tabs are recreated). */
    void EnsureInspectorTabLockMounted();

    /** The id this panel was registered with via DockingState::RegisterPanel. Used to
        find the matching dock tab (`tab:<id>`) when mounting the lock icon — the tab
        id uses the registered panel id, not the panel element's unique m_InstanceId. */
    void SetDockTabPanelId(const std::string& id) { m_DockTabPanelId = id; }
    
    // Script inspection API (shows variables and members from Script Editor)
    void ShowScriptVariables(const std::filesystem::path& scriptPath,
                             const std::vector<ScriptVariable>& variables,
                             const std::vector<ScriptMethod>& methods);
    
    // Update variable values (called when script is edited)
    void UpdateScriptVariableValues(const std::vector<ScriptVariable>& variables);
    
    // Highlight a variable field (called when caret is on value in script)
    void HighlightVariableField(const std::string& varName);

    /// Highlight the matching variable field and move keyboard focus to it (script caret/selection in value).
    void FocusScriptVariableField(const std::string& varName);
    
    // Clear variable field highlighting
    void ClearVariableFieldHighlight();
    
    // Set callback for when a variable value is edited in the Inspector
    void SetOnVariableEdited(std::function<void(const std::string& varName, const std::string& newValue)> cb) {
        m_OnVariableEdited = std::move(cb);
    }
    
    // Set callback for when a variable field is focused in the Inspector
    void SetOnVariableFocused(std::function<void(const std::string& varName)> cb) {
        m_OnVariableFocused = std::move(cb);
    }
    
    // Set callback for when a variable field loses focus
    void SetOnVariableUnfocused(std::function<void()> cb) {
        m_OnVariableUnfocused = std::move(cb);
    }

    // Set callback for when a script method is clicked in the members outline
    void SetOnScriptMethodNavigate(std::function<void(size_t lineNumber, const std::string& name)> cb) {
        m_OnScriptMethodNavigate = std::move(cb);
    }

    /** Apply toggle alignment (left/middle/right) by adding/removing CSS classes. */
    static void ApplyToggleAlign(InspectorPanel* panel, const std::string& value);

    /** Apply "solo section" mode where expanding one section collapses the others. */
    static void ApplySoloSections(InspectorPanel* panel, bool enabled);

    // Optional editor services (not owned).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }
    void SetChangeNotifications(Editor::EditorChangeNotifications* notifications) { m_ChangeNotifications = notifications; }
    void SetGetSelectedAssetPaths(std::function<std::vector<std::filesystem::path>()> cb) { m_GetSelectedAssetPaths = std::move(cb); }
    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn) { m_OpenColorPickerWindow = std::move(fn); }
    void SetOnGraphNodeParameterChanged(std::function<void(const std::string& nodeId,
                                                           const std::string& key,
                                                           const std::string& value,
                                                           bool commitUndo)> cb)
    {
        m_OnGraphNodeParameterChanged = std::move(cb);
    }
    void SetOnGraphVariableValueChanged(std::function<void(const std::string& variableName,
                                                           const std::string& value,
                                                           bool commitUndo)> cb)
    {
        m_OnGraphVariableValueChanged = std::move(cb);
    }
    void SetOnGraphTransitionChanged(std::function<void(const std::string& linkId,
                                                        const GraphTransitionDesc& desc,
                                                        bool commitUndo)> cb)
    {
        m_OnGraphTransitionChanged = std::move(cb);
    }
    void SetPingAsset(std::function<void(const std::filesystem::path&)> fn) { m_PingAsset = std::move(fn); }
    void SetSelectEntity(std::function<void(ECS::EntityHandle)> fn) { m_SelectEntity = std::move(fn); }
    void SetPingAssetPreserveInspector(std::function<void(const std::filesystem::path&)> fn)
    {
        m_PingAssetPreserveInspector = std::move(fn);
    }
    void SetOpenScript(std::function<void(const std::filesystem::path&)> fn) { m_OpenScript = std::move(fn); }
    void SetOpenMaterialGraph(std::function<void(const std::filesystem::path&)> fn)
    {
        m_OpenMaterialGraph = std::move(fn);
    }
    void SetOpenAsset(std::function<void(const std::filesystem::path&)> fn)
    {
        m_OpenAsset = std::move(fn);
    }
    void SetSoloKeepTransform(bool enabled);

    /// Sync scene + hierarchy when navigating inspector entity history (back/forward).
    void SetOnHistoryEntityNavigate(std::function<void(ECS::World*, ECS::EntityHandle)> cb)
    {
        m_OnHistoryEntityNavigate = std::move(cb);
    }
    void SetOnHistoryEntitiesNavigate(
        std::function<void(ECS::World*, const std::vector<ECS::EntityHandle>&)> cb)
    {
        m_OnHistoryEntitiesNavigate = std::move(cb);
    }
    void SetOnHistoryAssetNavigate(std::function<void(const std::vector<std::filesystem::path>&)> cb)
    {
        m_OnHistoryAssetNavigate = std::move(cb);
    }
    void SetOnHistoryScriptNavigate(std::function<void(const std::filesystem::path&)> cb)
    {
        m_OnHistoryScriptNavigate = std::move(cb);
    }

    void NavigateSelectionHistoryBack();
    void NavigateSelectionHistoryForward();

private:
    friend class Editor::InspectorVcsController;
    void ClearContent();
    // Snapshots the inspected entity's sorted, inspector-visible component signature into
    // m_LastComponentSignature. Hidden components (ECS::Disabled, Name, WorldTransform, …)
    // are excluded: they render no section, so their add/remove must not trip a rebuild —
    // the entity enable toggle and the lazy Name-add on first rename keystroke both depend
    // on that.
    void RefreshTrackedComponentSignature();
    // Shows the selected entity's activity once it changes: while the entity is off, itself or
    // through an ancestor, every section's dot is muted, and while an ancestor is the reason the
    // header says so (m_EntityActivity). Keyed on the entity's own state and its derived state
    // together, so a switch that leaves one of them unchanged still redraws.
    void SyncEntityActivityPresentation();
    // Rebuild only the named component's section body in place (header, collapse state,
    // and every other section keep their widgets). Used for ChangeKind::InspectorRebuild,
    // whose contract is "scene data unchanged, this component's rows changed shape".
    // Returns false when no such section exists; the caller falls back to a full rebuild.
    bool RebuildComponentSection(ECS::ComponentTypeId typeId);
    // Populate a section's body for the inspected entity by dispatching to the registered
    // inspector fn or the reflection-driven default inspector.
    void BuildComponentSectionBody(InspectorSection* section, ECS::ComponentTypeId typeId);
    void RefreshCurrentAssetInspector();
    // Rebuilds the inspected entity's sections at the next posted action, once however
    // often it is asked. A section's refresh callback asks for it while the panel runs
    // the callbacks the rebuild destroys, so the rebuild never runs inside that call.
    void RequestEntityRefresh();
    // Re-present the shown asset now that its reload has landed: the inspector's own
    // in-place update where it registered one, the full rebuild otherwise. A rebuild
    // is the blunt answer — it drops keyboard focus, closes an open popup and resets
    // the scroll, seconds after the edit that caused the reload.
    void ReShowAssetAfterItsReload();
    // An asset inspector reports what its asset currently holds — dimensions, format,
    // mip count — so a payload replaced in place leaves the panel describing the
    // previous one. An import setting that recooks the asset lands asynchronously,
    // which is where the contradiction shows: the row holds the value the user picked
    // while the lines around it still describe the artifact it replaced. These
    // re-present the shown asset when its reload lands. The subscription is installed
    // on the first asset shown and held until the panel dies: the panel alternates
    // between assets and entities constantly, and re-subscribing each time would churn
    // the dispatcher's lock for a callback that costs one enum compare.
    void EnsureShownAssetReloadSubscription();
    void ReleaseShownAssetReloadSubscription();
    void CommitGraphTransition(bool commitUndo);
    void AdjustValueByDrag(const std::string& varName, TextField* field, float deltaX);
    void WireSoloHandlersForSections();
    void ExecuteRemoveComponent(ECS::ComponentTypeId typeId);
    void EnsurePostProcessVolumeRemoveModal();
    void CaptureLoadedComponentValues(ECS::World* world,
                                      ECS::EntityHandle entity,
                                      const std::vector<ECS::ComponentTypeId>& componentTypeIds);
    bool TryGetLoadedComponentValues(ECS::World* world,
                                     ECS::EntityHandle entity,
                                     ECS::ComponentTypeId componentTypeId,
                                     std::vector<uint8_t>& outBytes) const;
    void ToggleComponentSectionSelection(ECS::ComponentTypeId typeId);
    void SelectComponentSectionRange(ECS::ComponentTypeId typeId);
    void SelectSingleComponentSection(ECS::ComponentTypeId typeId);
    void RefreshComponentSectionSelectionVisuals();
    std::vector<ECS::ComponentTypeId> GetContextCopySelection() const;
    void EnsureComponentSettingsStorageLoaded();
    void SaveComponentSettingsToProject();

    /// Reuses the InspectorSection header drag pipeline (ghost, drop indicator, DOM reorder).
    void BindSectionHeaderDragReorder(InspectorSection* section,
                                      UIElement* reorderRoot,
                                      bool restrictToMaterialSlotSections,
                                      std::function<void(UIElement* reorderedRoot)> onPersistOrder);
    /// Rebuilds m_ComponentOrder from the section DOM (top-level sections plus
    /// effects nested under the Post Process Volume section) and syncs the
    /// reorderable LDR post-FX StackOrder values to the visual order.
    void PersistComponentOrderFromSections();
    void WireMaterialSlotSectionDrag(InspectorSection* slotSection,
                                     UIElement* slotsContainer,
                                     uint32_t modelSlotIndex,
                                     const GUID& modelGuid,
                                     ECS::EntityHandle entityForOrder);
    
    // Search functionality
    void ApplySearchFilter(const std::string& searchText);
    void ClearSearchHighlights();
    std::string m_SearchFieldScope{"all"};

    // Inspector selection history — every state change (entity, asset, script, smart folder, empty)
    // is recorded; back/forward navigate through all of them.
    void ClearInspectorHistory();
    void RecordInspectorHistory(InspectorSelectionHistoryEntry entry);
    void NavigateInspectorHistoryAt(size_t index);
    void SyncInspectorHistoryButtonState();
    void MountHistoryButtons(UIElement& headerContainer);
    /// Minimal top header (title + arrows) mounted on m_TopRoot for non-entity / empty states
    /// so the inspector header strip is always present.
    void BuildSimpleTopHeader(const std::string& titleText,
                              const std::string& iconClass = {},
                              std::function<void(const std::string&)> onTitleChanging = {},
                              std::function<void(const std::string&)> onTitleChanged = {});

    /// Apply an asset-specific background (thumbnail if available) to the header icon host
    /// created by BuildSimpleTopHeader. Caller passes the pointer returned via m_HeaderIconHost.
    void ApplyAssetIconToHeader(UIElement* iconHost, const std::filesystem::path& assetPath);
    
    // Helper to strip suffixes from values for display (f/F for float, L/l for long, quotes for char/string)
    static std::string StripValueSuffix(const std::string& value);

    ScrollView* m_Scroll = nullptr;   // owned by UI tree
    UIElement* m_ContentRoot = nullptr; // container for dynamically built inspector UI
    UIElement* m_TopRoot = nullptr;     // fixed top area (outside scroll viewport)
    UIElement* m_HeaderIconHost = nullptr; // icon slot in the latest BuildSimpleTopHeader (not owned)
    TextField* m_HeaderTitleField = nullptr; // title field in the latest BuildSimpleTopHeader (not owned)
    UIElement* m_BottomRoot = nullptr;  // fixed bottom area (outside scroll viewport)
    SearchDialog* m_AddComponentDialog = nullptr; // non-owning; lives in root overlay
    std::shared_ptr<ComponentPresetSearchProvider> m_AddComponentProvider;
    ConfirmActionModal* m_RemovePostProcessConfirmModal = nullptr; // non-owning; child of UI root/panel
    ECS::ComponentTypeId m_PendingRemoveComponentTypeId{};
    UIElement* m_SearchBar = nullptr; // owned by UI tree
    TextField* m_SearchField = nullptr; // owned by UI tree
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    std::vector<ECS::EntityHandle> m_Entities; // all selected entities (multi-edit)
    // Shows the selected asset once its pending load has landed; drops the load when the
    // inspector has moved on to something else.
    void PollPendingAssetLoad();

    SharedPtr<Asset> m_SelectedAsset;
    // The load of a selected asset that was not loaded when it was selected: the inspector
    // says it is loading and shows the asset once the load lands (PollPendingAssetLoad),
    // never waiting for a cook or an import on the main thread.
    std::unique_ptr<AssetFuture> m_PendingAssetLoad;
    std::filesystem::path m_PendingAssetPath;
    // The asset whose load came back empty, so the next presentation reports the failure
    // rather than starting the load again.
    GUID m_FailedAssetLoad = GUID::Null();
    // 0 means no active subscription; the asset event dispatcher hands out monotonic
    // positive handles for AddCallback.
    uint32 m_ShownAssetReloadCallback = 0;
    // What the current asset inspector re-presents itself when its payload reloads.
    // Each returns whether it handled the reload; empty, or one answering false,
    // means the panel rebuilds instead. Cleared with the tree they point into.
    std::vector<std::function<bool()>> m_AssetReloadedCallbacks;
    std::vector<std::filesystem::path> m_LastSelectedAssetPaths;
    std::function<std::vector<std::filesystem::path>()> m_GetSelectedAssetPaths;
    std::unique_ptr<Editor::InspectorVcsController> m_VcsController;

    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
    std::function<void(const std::string& nodeId, const std::string& key, const std::string& value, bool commitUndo)> m_OnGraphNodeParameterChanged;
    std::function<void(const std::string& variableName, const std::string& value, bool commitUndo)> m_OnGraphVariableValueChanged;
    std::function<void(const std::string& linkId, const GraphTransitionDesc& desc, bool commitUndo)> m_OnGraphTransitionChanged;
    std::string m_GraphTransitionLinkId;
    std::string m_GraphTransitionFromName;
    std::string m_GraphTransitionToName;
    GraphTransitionDesc m_GraphTransitionDesc;
    std::function<void(const std::filesystem::path&)> m_PingAsset;
    std::function<void(ECS::EntityHandle)> m_SelectEntity;
    std::function<void(const std::filesystem::path&)> m_PingAssetPreserveInspector;
    std::function<void(const std::filesystem::path&)> m_OpenScript;
    std::function<void(const std::filesystem::path&)> m_OpenMaterialGraph;
    std::function<void(const std::filesystem::path&)> m_OpenAsset;

    // Optional context menu integration (native menus need a host window).
    const EditorContext* m_Context = nullptr; // not owned
    Platform::Window* m_Window = nullptr; // not owned
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
    ECS::ComponentTypeId m_ContextComponentTypeId{};

    // Keep the inspector UI consistent across undo/redo of structural changes
    // (component add/remove) by detecting signature changes for the active entity.
    Editor::EditorChangeNotifications::SubscriptionToken m_ComponentSub{};
    Editor::EditorChangeNotifications::SubscriptionToken m_StructureSub{};
    // Inspector-visible signature only (see RefreshTrackedComponentSignature).
    std::vector<ECS::ComponentTypeId> m_LastComponentSignature;

    // Raw pointer to the entity enable/disable toggle (owned by UI tree).
    Toggle* m_EntityToggle = nullptr;
    // Suppresses the SetOnValueChanged callback when syncing the toggle from external state.
    bool m_UpdatingToggle = false;
    // Whether the selected entity was active, on together with every ancestor, when the panel last
    // drew its sections (SyncEntityActivityPresentation).
    bool m_PresentedEntityActive = true;
    // The header's entity toggle and reason line, drawn from the entity's activity.
    Editor::InspectorEntityActivityPresenter m_EntityActivity;

    // Selection trail: every Show* state change is recorded for back/forward navigation.
    std::vector<InspectorSelectionHistoryEntry> m_InspectorSelectionHistory;
    size_t m_InspectorHistoryIndex = 0;
    bool m_HistoryHasCursor = false;
    bool m_SuppressInspectorHistory = false;
    // Coalesce rapid ShowEntity() calls dispatched from inside event handlers.
    // Without this, scenarios like rapid_select fire one PostAction per
    // selection event (~one per 16ms frame), and on the next tick every
    // queued call rebuilds the inspector — even though only the last is
    // visible. We collapse them: queue at most one PostAction at a time, and
    // have it read the latest pending request when it fires.
    bool m_ShowEntityCoalescePending = false;
    ECS::World* m_PendingShowWorld = nullptr;
    ECS::EntityHandle m_PendingShowEntity{};
    bool m_PendingShowForce = false;
    bool m_PendingShowSuppressHistory = false;
    bool m_PendingShowKeepMultiSelection = true;
    bool m_ModuleInspectorRefreshPending = false;
    bool m_EntityRefreshPosted = false;
    std::function<void(ECS::World*, ECS::EntityHandle)> m_OnHistoryEntityNavigate;
    std::function<void(ECS::World*, const std::vector<ECS::EntityHandle>&)> m_OnHistoryEntitiesNavigate;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnHistoryAssetNavigate;
    std::function<void(const std::filesystem::path&)> m_OnHistoryScriptNavigate;
    UIElement* m_HistoryButtonsRow = nullptr;
    Button* m_HistoryBackButton = nullptr;
    Button* m_HistoryForwardButton = nullptr;
    
    // Script variable callbacks
    std::function<void(const std::string& varName, const std::string& newValue)> m_OnVariableEdited;
    std::function<void(const std::string& varName)> m_OnVariableFocused;
    std::function<void()> m_OnVariableUnfocused;
    std::function<void(size_t lineNumber, const std::string& name)> m_OnScriptMethodNavigate;
    std::filesystem::path m_CurrentScriptPath;
    // Persist functions section collapse state across rebuilds for the same script
    bool m_FunctionsSectionExpanded = false;
    std::filesystem::path m_FunctionsSectionScriptPath;
    
    // Map of variable names to their TextField pointers (for updating values)
    std::unordered_map<std::string, TextField*> m_ScriptVariableFields;
    bool m_UpdatingFromScript = false; // Flag to prevent callback loops
    // Value column wrapper (.inspector-field) that gets inspector-field-highlighted when synced with script caret
    UIElement* m_HighlightedScriptValueChrome = nullptr;
    
    // Left mouse drag state for value adjustment (on variable name)
    bool m_LeftMouseDragging = false;
    UIElement* m_DraggingLabel = nullptr;
    TextField* m_DraggingField = nullptr;
    float m_DragStartX = 0.0f;
    std::string m_DragStartValue;
    std::string m_DragVariableName;
    
    // Smart folder inspector widget (owned by UI tree when added)
    SmartFolderInspector* m_SmartFolderInspector = nullptr;
    
    // Search state
    std::string m_CurrentSearchText;
    std::vector<UIElement*> m_SearchHighlightedElements;
    std::vector<UIElement*> m_SearchFilteredElements;

    // Audio asset preview: stop when inspector focus is lost (selection changes).
    Audio::AudioEmitterHandle m_AudioPreviewHandle{};

    // Live-refresh callbacks registered by component inspectors, keyed by the owning
    // section's component type so a single-section rebuild drops exactly that section's
    // callbacks (they capture raw widget pointers and would dangle otherwise).
    // TickSimulationRefresh() invokes Simulation at a throttled ~10 Hz cadence and
    // Frame every frame.
    struct SectionRefreshCallbacks
    {
        std::vector<std::function<void()>> Simulation;
        std::vector<std::function<void()>> Frame;
    };
    std::unordered_map<ECS::ComponentTypeId, SectionRefreshCallbacks> m_SectionRefreshCallbacks;
    std::chrono::steady_clock::time_point m_LastSimulationRefresh{};

    // When enabled, expanding one inspector section collapses the others.
    bool m_SoloSectionsEnabled = false;
    bool m_SoloKeepTransformEnabled = false;
    InspectorSection* m_TransformSection = nullptr;

    // Section drag-reorder state
    InspectorSection* m_SectionDragSource = nullptr;
    InspectorSection* m_SectionDragIndicatorTarget = nullptr;
    UIElement* m_SectionDragGhost = nullptr;
    UIElement* m_SectionDragHoverBlocker = nullptr;
    bool m_SectionDragActive = false;
    int m_SectionDropIndex = -1;
    /// Active drag list root: main inspector content for component sections, or the mesh material slots container for slot reorder.
    UIElement* m_SectionDragReorderRoot = nullptr;
    // Persisted component order for the inspected entity (survives ShowEntity rebuilds)
    ECS::EntityHandle m_ComponentOrderEntity{};
    std::vector<ECS::ComponentTypeId> m_ComponentOrder;
    std::unordered_map<InspectorSection*, ECS::ComponentTypeId> m_SectionTypeIds;
    std::unordered_map<ECS::ComponentTypeId, InspectorSection*> m_ComponentSectionsByType;
    std::unordered_set<ECS::ComponentTypeId> m_SelectedComponentTypes;
    std::vector<ECS::ComponentTypeId> m_ComponentSectionOrder;
    ECS::ComponentTypeId m_ComponentSelectionAnchor{};

    struct SavedComponentSettings
    {
        std::string Name;
        std::vector<uint8_t> Bytes;
    };
    std::unordered_map<ECS::ComponentTypeId, std::vector<SavedComponentSettings>> m_SavedComponentSettingsByType;
    // Maps dynamic context menu command offsets to saved setting slot indices.
    std::vector<size_t> m_ContextSavedSettingsMenuIndices;
    bool m_ComponentSettingsLoaded = false;
    std::filesystem::path m_ComponentSettingsWorkspaceRoot;

    // Baseline captured the first time a loaded scene component is inspected.
    // Context-menu Reset restores this imported/loaded state instead of the
    // component type's generic constructor defaults.
    struct EntityComponentStateKey
    {
        uint64_t WorldId = 0;
        uint32_t EntityId = 0;
        ECS::ComponentTypeId TypeId{};

        bool operator==(const EntityComponentStateKey& other) const
        {
            return WorldId == other.WorldId && EntityId == other.EntityId && TypeId == other.TypeId;
        }
    };
    struct EntityComponentStateKeyHash
    {
        std::size_t operator()(const EntityComponentStateKey& key) const
        {
            std::size_t hash = std::hash<uint64_t>{}(key.WorldId);
            hash ^= std::hash<uint32_t>{}(key.EntityId) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            hash ^= std::hash<ECS::ComponentTypeId>{}(key.TypeId) + 0x9e3779b9u +
                    (hash << 6u) + (hash >> 2u);
            return hash;
        }
    };
    std::unordered_map<EntityComponentStateKey,
                       std::vector<uint8_t>,
                       EntityComponentStateKeyHash> m_LoadedComponentValues;

    // Persisted section collapse states across inspector rebuilds without leaking
    // one entity's section choices into another entity that shares the component.
    std::unordered_map<EntityComponentStateKey, bool, EntityComponentStateKeyHash> m_SectionCollapseStates;

    std::unordered_map<InspectorSection*, uint32_t> m_MaterialSlotSectionIndices;
    std::unordered_map<MaterialSlotOrderKey, std::vector<uint32_t>, MaterialSlotOrderKeyHash> m_MeshRendererMaterialSlotDisplayOrder;

    // Lock: keeps the current inspected object when selection changes elsewhere.
    bool m_Locked = false;
    UIElement* m_LockButton = nullptr;

    // True while the content is a graph node (set by ShowGraphNode, dropped by
    // every ClearContent) — the gate for ClearGraphNodeIfShown.
    bool m_ShowingGraphNode = false;

    // Id used by the dockspace for tab:<id>. Set by EditorApplication after RegisterPanel.
    std::string m_DockTabPanelId;

    // Unique identifier for this inspector instance (supports multiple inspector panels).
    std::string m_InstanceId;
    static int s_InspectorInstanceCounter;
};

} // namespace GameEngine

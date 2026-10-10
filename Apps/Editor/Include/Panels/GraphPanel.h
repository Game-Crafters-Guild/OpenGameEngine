#pragma once

#include "Graph/GraphModel.h"
#include "Graph/GraphCanvas.h"
#include "Graph/GraphNest.h"
#include "Graph/GraphTransitionStore.h"
#include "Panels/GraphKindHooks.h"
#include "InspectorRegistry.h"
#include "UI/Controls/DockPanel.h"
#include "UI/UIElement.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "UndoRedo/UndoRedoService.h"

namespace GameEngine {

class Button;
class ConfirmActionModal;
class Dropdown;
class INativeContextMenu;
class Label;
class SaveSceneChangesModal;
struct EditorContext;
class ScrollView;
class TextField;

/**
 * Base editor panel for node-based graphs. Kind is fixed at construction;
 * CreateForKind returns ShaderGraphPanel, AnimationGraphPanel, or
 * GameLogicGraphPanel. OpenGraph refuses a file whose KindId does not match
 * PanelKindId — the host routes by the document's KindId to the matching dock.
 * Kind-specific preview and runtime visualization live on the subclasses.
 * Compile lives on ShaderGraphPanel. The base has no KindId tests.
 * Subclasses listen to GraphPanelEvent rather than GraphPanel deciding a
 * kind refresh inside MarkDirty.
 */
class GraphPanel : public DockPanel {
public:
    /** A moment in the panel's life that a kind can react to. This is the home
        for anything the panel only announces: it carries no arguments and no
        answer, so it needs no entry in GraphKindHooks. */
    enum class GraphPanelEvent : std::size_t
    {
        ModelChanged = 0,
        Opened,
        Shown,
        Saved,
        ContextChanged,
        NodeDragEnded,
        PostLayout,
        CanvasPrimaryPress,
        ExpandedNodesToggled,
        VariablesEdited,
        Count
    };

    std::string_view DeclaredTabIconClass() const override { return "node-icon"; }

    static std::unique_ptr<GraphPanel> CreateForKind(std::string_view kindId);
    static void ForEachLive(const std::function<void(GraphPanel&)>& fn);
    static GraphPanel* FindLiveByKind(std::string_view kindId);
    static GraphPanel* FindLiveByPath(const std::filesystem::path& path);
    static GraphPanel* FindEmptyLiveByKind(std::string_view kindId);
    static GraphPanel* FindLiveLastOpened();
    void NoteAsLastOpened();
    /** The panel that last presented a node/transition to the inspector. */
    static GraphPanel* InspectorTarget();
    static bool TryReadKindId(const std::filesystem::path& path, std::string& outKindId);

    GraphPanel() = delete;
    ~GraphPanel() override;

protected:
    explicit GraphPanel(std::string_view kindId);
    void SetupUI();

    /** What this graph KIND contributes. A kind fills the entries it needs,
        once, and the panel calls one only when it was filled — so the shared
        panel carries no vocabulary for any one kind. */
    void SetKindHooks(GraphKindHooks hooks) { m_KindHooks = std::move(hooks); }
    const GraphKindHooks& KindHooks() const { return m_KindHooks; }

    /** The window the panel opens for a colour swatch. Kinds that have
        colours route their own nodes to it. */
    const OpenColorPickerWindowFn& OpenColorPickerWindow() const { return m_OpenColorPickerWindow; }

    void Listen(GraphPanelEvent e, std::function<void()> handler);
    void Notify(GraphPanelEvent e);

    GraphNestKind CurrentNestKind() const;
    void MarkDirty();
    void WriteLiveNestToHosts();
    bool CaptureGraphSnapshot(std::vector<std::uint8_t>& out) const;
    void CommitLiveGraphMutation(const char* actionName, std::function<void()> mutate);

    Graph::Model& RootModel() { return m_Model; }
    const Graph::Model& RootModel() const { return m_Model; }
    const EditorContext* Context() const { return m_Context; }
    GraphCanvas* Canvas() { return m_Canvas; }
    const GraphCanvas* Canvas() const { return m_Canvas; }
    bool ExpandedNodesEnabled() const { return m_ExpandedNodes; }
    bool NodeDragInProgress() const { return m_NodeDragInProgress; }
    const std::vector<GraphNestFrame>& NestStack() const { return m_Nest; }

public:
    static bool GetPanelScrollbarsPreference();
    static void SetPanelScrollbarsPreference(bool enabled);
    static bool GetVariablesScrollbarsPreference();
    static void SetVariablesScrollbarsPreference(bool enabled);

    std::string_view PanelKindId() const { return m_PanelKindId; }

    using RequestOpenGraphAssetFn = std::function<bool(const std::filesystem::path&)>;
    void SetRequestOpenGraphAsset(RequestOpenGraphAssetFn fn) { m_RequestOpenGraphAsset = std::move(fn); }

    /**
     * Open a graph file for editing. Files whose KindId does not match this
     * panel are forwarded through SetRequestOpenGraphAsset.
     * @return true if the file was loaded successfully
     */
    bool OpenGraph(const std::filesystem::path& path);

    /**
     * Save the current graph to disk.
     * @return true if saved successfully
     */
    bool SaveGraph();

    /**
     * Check if there are unsaved changes.
     */
    bool HasUnsavedChanges() const { return m_Dirty; }
    void PromptSaveBeforeQuit(std::function<void()> onProceed);

    /**
     * Get the currently open graph path.
     */
    const std::filesystem::path& GetCurrentGraphPath() const { return m_CurrentPath; }

    /**
     * Set callback when graph is saved.
     */
    void SetOnGraphSaved(std::function<void(const std::filesystem::path&)> cb) { m_OnGraphSaved = std::move(cb); }

    /** Set undo/redo service for undoable graph edits (add/delete node, link, move, paste). */
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn);

    /** Set callback for node selection (Editor wires this to Inspector panel). */
    using OnNodeSelectedFn = std::function<void(const std::string& nodeId, const std::string& nodeTypeId,
                                                 const std::string& displayName,
                                                 const std::unordered_map<std::string, std::string>& parameters,
                                                 const std::string& kindId)>;
    void SetOnNodeSelected(OnNodeSelectedFn cb) { m_OnNodeSelected = std::move(cb); }

    /** Set callback for node deselection — the canvas selection emptied or the
        model was swapped, so a node shown outside the panel (Inspector) is stale. */
    void SetOnNodeDeselected(std::function<void()> cb) { m_OnNodeDeselected = std::move(cb); }

    using OnTransitionSelectedFn = std::function<void(const std::string& linkId,
                                                       const std::string& fromName,
                                                       const std::string& toName,
                                                       const GraphTransitionDesc& desc)>;
    void SetOnTransitionSelected(OnTransitionSelectedFn cb) { m_OnTransitionSelected = std::move(cb); }

    /** Restore graph state from snapshot and refresh canvas (used by undo/redo). */
    bool ApplyGraphSnapshot(const std::vector<std::uint8_t>& snapshot);
    bool SetNodeParameter(const std::string& nodeId, const std::string& key, const std::string& value,
                          bool commitUndo = true);
    bool EditGraphVariableFromInspector(const std::string& name, const std::string& value, bool commitUndo = true);
    bool SetTransitionFromInspector(const std::string& linkId, const GraphTransitionDesc& desc,
                                    bool commitUndo = true);

    void SetContext(const EditorContext* ctx);
    /** Model of the graph currently in view (the deepest open nest). */
    Graph::Model* ActiveGraphModel();
    const Graph::Model* ActiveGraphModel() const;
    /** Per-frame update. Computes its own delta time (the panel update callback
        has no frame clock). */
    void Update();

    void OnPostLayout() override;

private:
    void RebuildPalette();
    void SchedulePaletteRebuild();
    void RebuildVariablesPanel();
    void ScheduleVariablesPanelRebuild();
    void AddNodeFromPalette(const std::string& typeId);
    void AddNodeFromPaletteAtScreen(const std::string& typeId, float screenX, float screenY,
                                    bool centerOnPointer, bool rejectPaletteArea);
    void AddNodeFromWireDropAtScreen(const std::string& anchorNodeId, const std::string& anchorPortId,
                                     const std::string& typeId, const std::string& connectPortId,
                                     float screenX, float screenY, bool fromInputPort);
    void ScheduleLoadTestGraph();
    void LoadTestGraph();
    void OnSaveButtonClicked();
    void OnNewGraphButtonClicked();
    void CreateNewGraph(const std::filesystem::path& path);
    std::filesystem::path PromptSaveAsPath();
    void DiscardCurrentGraphChanges();
    void RefreshGraphViewAfterModelChange();
    void EnsureUnsavedChangesModal();
    void UpdateTitleLabel();
    void SyncViewportFromModel();
    void SyncViewportToModel();
    void WriteNestFramesInto(Graph::Model& root) const;
    void EnterNest(const std::string& nodeId);
    void ExitNest(bool writeBack);
    void ExitAllNests(bool writeBack);
    /** `host` is the node the panel just entered, and is null when it left one
        or stayed at the top level. */
    void ApplyNestView(const Graph::Node* host = nullptr);
    bool IsPaletteCategoryVisible(std::string_view category) const;
    void OnCanvasNodeDoubleClicked(const std::string& nodeId);
    /** Updates palette position during title-bar drag (threshold, clamp, style overrides). */
    void ApplyNodePaletteDrag(float x, float y);
    void ApplyNodePaletteCollapsedState();
    void BeginNodePaletteResize(float x, float y);
    void UpdateNodePaletteResize(float x, float y);
    void EndNodePaletteResize();
    void ApplyVariablesPanelBounds();
    void ApplyVariablesPanelDrag(float x, float y);
    void ApplyVariablesPanelCollapsedState();
    void BeginVariablesPanelResize(float x, float y);
    void UpdateVariablesPanelResize(float x, float y);
    void EndVariablesPanelResize();
    void ApplyPanelScrollbarPreference();
    void UpdatePanelOverflowIndicators();
    void SyncCanvasNodeFactory();
    /** The kind's node factory, registering its node types on first use. */
    const GraphCanvas::NodeFactoryFn& KindNodeFactory();
    void SyncViewStateToolbar();
    void OnCanvasSelectionChanged(const std::string& nodeId);
    void OnCanvasLinkSelected(const std::string& linkId);
    void OnCanvasContextMenu(float screenX, float screenY, bool onNode, const std::string& nodeId);
    void OnCanvasWireDropMenu(float screenX, float screenY,
                              const std::string& anchorNodeId, const std::string& anchorPortId,
                              bool fromInputPort);

    void ToggleExpandedNodes();
    void UpdateExpandedNodesToggleButton();
    void ToggleNodePalettePanel();
    void UpdateNodePalettePanelToggleButton();
    void ToggleVariablesPanel();
    void UpdateVariablesPanelToggleButton();
    void TogglePaletteNodeColors();
    void UpdatePaletteNodeColorToggleButton();
    /** Point the connection-style toggle at the icon for the canvas's current style. */
    void SyncConnectionStyleButton();
    void ApplyNodePaletteBounds();
    enum class VariablesSortMode
    {
        Created,
        Name,
        Custom
    };
    void ShowVariablesSortContextMenu(float screenX, float screenY);
    void SetVariablesSortMode(VariablesSortMode mode);
    void SetVariablesSortAscending(bool ascending);
    std::uint64_t NextGraphVariableCreatedOrder() const;
    void AssignMissingVariableCreatedOrders();
    void OnLoadGraphButtonClicked();
    bool ReorderGraphVariable(const std::string& name, size_t targetDisplayIndex);
    Graph::Variable* EnsureGraphVariable(const std::string& name);
    Graph::Variable* FindGraphVariable(const std::string& name);
    const Graph::Variable* FindGraphVariable(const std::string& name) const;
    std::vector<Graph::Variable> CollectVariablesForDisplay() const;
    void AddGraphVariable();
    void RenameGraphVariable(const std::string& oldName, const std::string& newName);
    void RequestSetGraphVariableType(const std::string& name, const std::string& type);
    void SetGraphVariableType(const std::string& name, const std::string& type);
    bool ApplyGraphVariableValue(const std::string& name,
                                 const std::string& value,
                                 bool syncPublicMaterials);
    bool SetGraphVariableValue(const std::string& name, const std::string& value, bool commitUndo);
    void SetGraphVariablePublic(const std::string& name, bool isPublic);
    void SetGraphVariableGlobal(const std::string& name, bool isGlobal);
    int CountGraphVariableUsages(const std::string& name) const;
    void EnsureVariableTypeWarningModal();
    void ShowPaletteNodeDragGhost(const std::string& typeId, float screenX, float screenY);
    void UpdatePaletteNodeDragGhost(float screenX, float screenY);
    void HidePaletteNodeDragGhost();
    bool ApplyNodeParameterValue(const std::string& nodeId, const std::string& key,
                                 const std::string& value);
    Editor::UndoRedoService::InteractiveEdit BeginGraphParameterEdit(const char* actionName);
    Editor::UndoRedoService::InteractiveEdit BeginGraphVariableEdit(const char* actionName);

    static bool MatchSearch(const std::string& displayName, const std::string& typeId,
                            const std::string& search);

    const EditorContext* m_Context = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
    Editor::UndoRedoService::InteractiveEdit m_GraphDragEdit{};

    Editor::UndoRedoService::InteractiveEdit m_GraphParameterEdit{};
    std::string m_GraphParameterEditNodeId;
    std::string m_GraphParameterEditKey;
    Editor::UndoRedoService::InteractiveEdit m_GraphVariableEdit{};
    std::string m_GraphVariableEditName;
    OnNodeSelectedFn m_OnNodeSelected;  // callback for node selection
    std::function<void()> m_OnNodeDeselected;
    OnTransitionSelectedFn m_OnTransitionSelected;
    Editor::UndoRedoService::InteractiveEdit m_GraphTransitionEdit{};
    std::string m_GraphTransitionEditLinkId;

    Graph::Model m_Model;
    std::string m_PanelKindId;
    RequestOpenGraphAssetFn m_RequestOpenGraphAsset;
    bool m_ForwardingOpenGraph = false;
    std::vector<GraphNestFrame> m_Nest;
    std::filesystem::path m_CurrentPath;
    bool m_Dirty = false;
    std::array<std::vector<std::function<void()>>,
               static_cast<std::size_t>(GraphPanelEvent::Count)>
        m_EventListeners;

    SaveSceneChangesModal* m_UnsavedChangesModal = nullptr;
    ConfirmActionModal* m_VariableTypeWarningModal = nullptr;
    std::filesystem::path m_PendingLoadGraphPath;
    bool m_HasPendingLoadGraph = false;
    bool m_HasPendingNewGraph = false;
    std::filesystem::path m_PendingNewGraphPath;
    std::function<void()> m_PendingQuitCallback;
    std::string m_PendingVariableTypeName;
    std::string m_PendingVariableTypeValue;

    GraphKindHooks m_KindHooks;
    /** Filled from GraphKindHooks::NodeCatalog on first use; the call also
        registers the kind's node types. */
    GraphCanvas::NodeFactoryFn m_KindNodeFactory;
    GraphCanvas* m_Canvas = nullptr;
    Button* m_SaveButton = nullptr;
    Label* m_SaveDirtyIndicator = nullptr;
    Button* m_GridToggleButton = nullptr;
    Button* m_ConnectionStyleButton = nullptr;
    Label* m_TitleLabel = nullptr;
    Label* m_SubgraphBackLabel = nullptr;
    Button* m_ExpandedNodesToggleButton = nullptr;
    Button* m_NodePalettePanelToggleButton = nullptr;
    Button* m_VariablesPanelToggleButton = nullptr;
    Button* m_PaletteNodeColorToggleButton = nullptr;

    UIElement* m_PaletteContent = nullptr;
    UIElement* m_Body = nullptr;
    ScrollView* m_PaletteScroll = nullptr;
    TextField* m_PaletteSearchField = nullptr;
    TextField* m_GraphSearchField = nullptr;
    UIElement* m_Palette = nullptr;
    UIElement* m_PaletteDragHandle = nullptr;
    UIElement* m_PaletteResizeHandle = nullptr;
    UIElement* m_PaletteTopOverflowIndicator = nullptr;
    UIElement* m_PaletteBottomOverflowIndicator = nullptr;
    UIElement* m_VariablesPanel = nullptr;
    UIElement* m_VariablesDragHandle = nullptr;
    UIElement* m_VariablesResizeHandle = nullptr;
    UIElement* m_VariablesTopOverflowIndicator = nullptr;
    UIElement* m_VariablesBottomOverflowIndicator = nullptr;
    UIElement* m_VariablesContent = nullptr;
    ScrollView* m_VariablesScroll = nullptr;
    TextField* m_VariablesSearchField = nullptr;
    UIElement* m_PaletteNodeDragGhost = nullptr;
    Label* m_PaletteNodeDragGhostLabel = nullptr;
    Label* m_PaletteNodeDragGhostValueLabel = nullptr;
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
    std::unique_ptr<INativeContextMenu> m_VariablesContextMenu;

    struct WireDropMenuItem
    {
        uint32_t CommandId = 0;
        std::string TypeId;
        std::string ConnectPortId;
        bool FromInputPort = false;
    };
    struct CanvasContextMenuItem
    {
        uint32_t CommandId = 0;
        std::string TypeId;
    };
    std::vector<WireDropMenuItem> m_WireDropMenuItems;
    std::vector<CanvasContextMenuItem> m_CanvasContextMenuItems;
    std::string m_WireDropAnchorNodeId;
    std::string m_WireDropAnchorPortId;
    bool m_WireDropFromInputPort = false;
    float m_WireDropScreenX = 0.f;
    float m_WireDropScreenY = 0.f;
    float m_CanvasContextMenuScreenX = 0.f;
    float m_CanvasContextMenuScreenY = 0.f;

    bool m_NodeDragInProgress = false;
    /** Node-side edit happened; the inspector mirror delivers from Update
        once no pointer gesture holds the mouse. */
    bool m_InspectorRefreshDeferred = false;
    /** Set by the drag-ended callback so the change it reports next is known to
        be a move; cleared by that report. */
    bool m_ChangeFromNodeMove = false;

    /** Expanded node view; pushed to the canvas, which owns the behavior. */
    bool m_ExpandedNodes = false;
    bool m_ColorPaletteNodes = false;
    bool m_NodePalettePanelVisible = true;
    bool m_VariablesPanelVisible = true;
    bool m_LastPanelScrollbarsVisible = true;
    bool m_LastVariablesScrollbarsVisible = true;
    VariablesSortMode m_VariablesSortMode = VariablesSortMode::Name;
    bool m_VariablesSortAscending = true;

    bool m_PaletteRebuildScheduled = false; /* At most one ScheduleNext in flight so typing doesn't hang */
    bool m_VariablesRebuildScheduled = false;
    bool m_FrameNodesOnNextLayout = false; /* Frame all nodes to fit view when opening a graph */
    int m_FrameRetries = 0; /* Limit retries so stale flag doesn't fire on later interactions */
    std::chrono::steady_clock::time_point m_UpdateLastTick{};

    bool m_DraggingPalette = false;
    bool m_PaletteDragPending = false; /* MouseDown on title; set m_DraggingPalette only after move threshold */
    float m_PaletteDragStartX = 0.f;
    float m_PaletteDragStartY = 0.f;
    float m_PaletteDragStartLeft = 0.f;
    float m_PaletteDragStartTop = 0.f;
    bool m_ResizingPalettePanel = false;
    float m_PaletteResizeStartY = 0.f;
    float m_PaletteResizeStartHeight = 0.f;

    bool m_PaletteNodeDragPending = false;
    bool m_DraggingPaletteNode = false;
    std::string m_PaletteNodeDragTypeId;
    float m_PaletteNodeDragStartX = 0.f;
    float m_PaletteNodeDragStartY = 0.f;

    bool m_PaletteCollapsed = false;
    float m_PaletteUserHeight = -1.f;
    float m_LastAppliedPaletteHeight = -1.f;
    bool m_PaletteHeightOverrideApplied = false;
    std::chrono::steady_clock::time_point m_PaletteTitleLastClickTime{};

    bool m_DraggingVariablesPanel = false;
    bool m_VariablesDragPending = false;
    float m_VariablesDragStartX = 0.f;
    float m_VariablesDragStartY = 0.f;
    float m_VariablesDragStartLeft = 0.f;
    float m_VariablesDragStartTop = 0.f;
    bool m_ResizingVariablesPanel = false;
    float m_VariablesResizeStartY = 0.f;
    float m_VariablesResizeStartHeight = 0.f;
    bool m_VariablesCollapsed = false;
    bool m_VariableRowDragPending = false;
    bool m_DraggingVariableRow = false;
    std::string m_VariableRowDragName;
    float m_VariableRowDragStartY = 0.f;
    size_t m_VariableRowDragTargetIndex = 0;
    float m_VariablesUserHeight = -1.f;
    float m_LastAppliedVariablesHeight = -1.f;
    bool m_VariablesHeightOverrideApplied = false;
    std::chrono::steady_clock::time_point m_VariablesTitleLastClickTime{};

    std::function<void(const std::filesystem::path&)> m_OnGraphSaved;
};

} // namespace GameEngine

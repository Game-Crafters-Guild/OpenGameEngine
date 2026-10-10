#pragma once

#include "Graph/GraphRouteSolver.h"
#include "InspectorRegistry.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNodeMetrics.h"
#include "Mathematics/Vector2.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>

namespace GameEngine {

class AssetRegistry;
class GraphNodePool;
class GraphOverlay;
class GraphPortedNode;
class IThumbnailProvider;
struct GraphNodeEditHost;

/**
 * Custom UI element for editing a node graph: pan/zoom, draw nodes and links,
 * hit-test, selection, node move, and connection drag.
 */
class GraphCanvas : public UIElement {
public:
    /** The last press the canvas took, so a second one on the same target can be
        recognised as a double click. An empty PortId means the node body. */
    struct PressTracker
    {
        std::string NodeId;
        std::string PortId;
        std::chrono::steady_clock::time_point At{};
    };

    /** Grid unit size in graph space; node size and snap align to this. */
    static constexpr float kGridSizeGraph = GraphNodeMetrics::kGridSizeGraph;
    /** Node size is always a multiple of grid unit for alignment. */
    static constexpr float kNodeWidth = GraphNodeMetrics::kNodeWidth;
    static constexpr float kNodeHeight = GraphNodeMetrics::kNodeHeight;
    static constexpr float kNodeTitleRowHeightGraph = GraphNodeMetrics::kNodeTitleRowHeightGraph;
    static constexpr float kNodePortAreaTopPaddingGraph = GraphNodeMetrics::kNodePortAreaTopPaddingGraph;
    static constexpr float kNodePortAreaBottomPaddingGraph = GraphNodeMetrics::kNodePortAreaBottomPaddingGraph;
    static constexpr float kPortRadius = GraphNodeMetrics::kPortSizeGraph * 0.5f;
    /** Hit-test radius for ports (graph space); larger than kPortRadius for easier clicking. */
    static constexpr float kPortHitRadius = 12.f;
    static constexpr float kPortSpacing = GraphNodeMetrics::kPortSpacing;
    /** Minimum pointer movement (screen px) before a node mousedown is committed as a drag. */
    static constexpr float kNodeDragThresholdPx = 4.f;
    /** Smallest zoom that still keeps a 13px-at-1x title around 7.6px. */
    static constexpr float kMinZoom = 0.585f;
    static constexpr float kMaxZoom = 3.f;
    static constexpr const char* kNodeCornerRadiusPreference = "nodeGraph.nodeCornerRadius";
    static constexpr float kDefaultNodeCornerRadius = 4.f;
    static constexpr float kMinNodeCornerRadius = 0.f;
    static constexpr float kMaxNodeCornerRadius = 16.f;
    static constexpr const char* kNodeHeaderAlignmentPreference = "nodeGraph.headerAlignment";
    static constexpr const char* kConnectionRoundedCornersPreference = "nodeGraph.connectionRoundedCorners";
    /** How the wires of a selected node stand out: "off", "bright", "glow" or "halo". */
    static constexpr const char* kSelectedNodeWireEmphasisPreference = "nodeGraph.selectedNodeWireEmphasis";
    static constexpr const char* kNodeDropShadowsPreference = "nodeGraph.dropShadows";
    static constexpr const char* kNodeDropShadowOffsetXPreference = "nodeGraph.dropShadow.offsetX";
    static constexpr const char* kNodeDropShadowOffsetYPreference = "nodeGraph.dropShadow.offsetY";
    static constexpr const char* kNodeDropShadowBlurPreference = "nodeGraph.dropShadow.blur";
    static constexpr const char* kNodeDropShadowOpacityPreference = "nodeGraph.dropShadow.opacity";
    static constexpr float kDefaultNodeDropShadowOffsetX = 4.0f;
    static constexpr float kDefaultNodeDropShadowOffsetY = 8.0f;
    static constexpr float kDefaultNodeDropShadowBlur = 24.0f;
    static constexpr float kDefaultNodeDropShadowOpacity = 0.2f;
    static float GetNodeCornerRadius();
    static void SetNodeCornerRadius(float radius);
    static bool GetConnectionRoundedCorners();
    static void SetConnectionRoundedCorners(bool enabled);
    static bool GetNodeDropShadows();
    static void SetNodeDropShadows(bool enabled);
    static void SetNodeDropShadowStyle(float offsetX, float offsetY, float blur, float opacity);
    static std::string GetNodeHeaderAlignment();
    static void SetNodeHeaderAlignment(const std::string& alignment);
    static std::string GetSelectedNodeWireEmphasis();
    static void SetSelectedNodeWireEmphasis(const std::string& emphasis);
    // Open graphs paint radius, alignment, shadows and node colors from
    // process statics; call after those change so this frame picks them up.
    static void MarkLiveCanvasesDirty();
    static float SnapGraphCoordinate(float value);
    static void SnapGraphPosition(float& x, float& y);

    /** Port-area height. Port placement measures against this, never against a
        rect grown by detail rows, so expanding a node cannot move its wires.
        Graph units, like every geometry accessor here — screen-space values
        carry a Px suffix (kNodeDragThresholdPx, ConnectionStubOffsetPx). */
    static float GetNodeBaseHeight(const Graph::Node& node)
    {
        return GraphNodeMetrics::GetNodeBaseHeight(node);
    }

    static float GetNodeWidth(const Graph::Node& node)
    {
        return GraphNodeMetrics::GetNodeWidth(node);
    }

    /** Height of the node's whole rect, in graph units, honoring this canvas's
        expanded view — the port area plus any detail rows and reserved block.
        Contrast GetNodeBaseHeight, which is the port area alone: port placement
        measures against the base so growing the rect cannot move the wires.
        Instance-level because the answer depends on canvas state and on the
        node kind's inline-editor rule, neither of which a static can see. */
    float GetNodeRectHeight(const Graph::Node& node) const;

    static float PortTopPercent(int index, int count, float nodeHeightGraph)
    {
        return GraphNodeMetrics::PortTopPercent(index, count, nodeHeightGraph);
    }

    explicit GraphCanvas(Graph::Model* model);
    ~GraphCanvas() override;

    void SetModel(Graph::Model* model);
    Graph::Model* GetModel() const { return m_Model; }

    /** Builds the node elements the pool hands out; the host installs a factory
        producing the graph kind's GraphPortedNode subclass. Empty = plain
        GraphPortedNode (the default). Installing destroys pooled nodes built by
        the previous factory and rebuilds them through the new one. */
    using NodeFactoryFn = std::function<std::unique_ptr<GraphPortedNode>()>;
    void SetNodeFactory(NodeFactoryFn factory);

    /** Asset services handed to node widgets through the edit host — texture
        drop slots use them for the click-to-pick search dialog. Wired by the
        panel when its editor context arrives (both null before that). */
    void SetAssetServices(AssetRegistry* registry, IThumbnailProvider* thumbnails)
    {
        m_AssetRegistry = registry;
        m_ThumbnailProvider = thumbnails;
    }

    /** Expanded node view: parameters that get no inline port editor are drawn
        as value rows under the port area, growing the node rect. Ports keep
        their collapsed positions, so wires do not move. Growing the rect
        re-runs the overlap-free resolve (same invariant as SetModel). */
    void SetExpandedNodes(bool expanded);
    bool GetExpandedNodes() const { return m_ExpandedNodes; }

    /** Rebind node widgets to current model values without firing the
        graph-changed callback — the path for edits made outside the canvas
        (inspector, or a host whose per-node visuals moved). */
    void RefreshBoundNodeValues();

    /** Hide-first delete: the pooled widgets and their wires vanish this
        frame, the model mutation is deferred past dispatch — mutating the
        model while its elements are dispatching destroys them mid-event. */
    void DeleteSelectedNodes();
    /** Hide-first delete of the selected link (keyboard Delete with no nodes). */
    void DeleteSelectedLink();
    /** Host calls once per frame, outside dispatch. */
    void FlushDeferredModelMutation();

    void SetPanZoom(float panX, float panY, float zoom);
    void GetPanZoom(float& panX, float& panY, float& zoom) const;
    /** Set pan/zoom so all nodes fit in the visible canvas (e.g. after opening a graph). */
    void FrameNodesToFit();

    std::string GetSelectedNodeId() const;
    void SetSelectedNodeId(const std::string& id);
    const std::unordered_set<std::string>& GetSelectedNodeIds() const { return m_SelectedNodeIds; }
    void SetSelectedNodeIds(const std::unordered_set<std::string>& ids);
    bool IsNodeSelected(const std::string& nodeId) const { return m_SelectedNodeIds.count(nodeId) != 0; }

    using OnSelectionChangedFn = std::function<void(const std::string& nodeId)>;
    void SetOnSelectionChanged(OnSelectionChangedFn fn) { m_OnSelectionChanged = std::move(fn); }

    using OnLinkSelectedFn = std::function<void(const std::string& linkId)>;
    void SetOnLinkSelected(OnLinkSelectedFn fn) { m_OnLinkSelected = std::move(fn); }
    std::string GetSelectedLinkId() const { return m_SelectedLinkId; }
    void SetSelectedLinkId(const std::string& id);

    using OnRequestContextMenuFn = std::function<void(float screenX, float screenY, bool onNode, const std::string& nodeId)>;
    void SetOnRequestContextMenu(OnRequestContextMenuFn fn) { m_OnRequestContextMenu = std::move(fn); }

    using OnRequestWireDropMenuFn = std::function<void(float screenX, float screenY,
                                                       const std::string& anchorNodeId,
                                                       const std::string& anchorPortId,
                                                       bool fromInputPort)>;
    void SetOnRequestWireDropMenu(OnRequestWireDropMenuFn fn) { m_OnRequestWireDropMenu = std::move(fn); }

    using OnGraphChangedFn = std::function<void()>;
    void SetOnGraphChanged(OnGraphChangedFn fn) { m_OnGraphChanged = std::move(fn); }

    /** Wrap a mutation in an undoable scope (name + lambda). If not set, mutation runs without undo. */
    using UndoScopeFn = std::function<void(const std::string& actionName, std::function<void()> mutate)>;
    void SetUndoScope(UndoScopeFn fn) { m_UndoScope = std::move(fn); }
    /** Called when a node drag starts (so the host can begin an interactive undo edit). */
    void SetOnNodeDragStarted(std::function<void()> fn) { m_OnNodeDragStarted = std::move(fn); }
    /** Called when a node drag ends (so the host can commit the interactive edit). */
    void SetOnNodeDragEnded(std::function<void()> fn) { m_OnNodeDragEnded = std::move(fn); }

    /** Double-click on a node body (empty portId), not a port, and not a drag past kNodeDragThresholdPx. */
    void SetOnNodeDoubleClicked(std::function<void(const std::string& nodeId)> fn)
    {
        m_OnNodeDoubleClicked = std::move(fn);
    }

    /**
     * Every primary-button press on the canvas, reported before the canvas
     * decides what the press means (node drag, link drag, marquee).
     *
     * The canvas consumes its own presses, so a host that needs to know a click
     * happened cannot learn it from the application's InputSystem — a consumed
     * press never reaches it. This is the canvas telling its host directly,
     * which is also the only report that is correctly scoped: it fires for
     * presses on THIS canvas and no other.
     */
    void SetOnPrimaryPress(std::function<void()> fn) { m_OnPrimaryPress = std::move(fn); }

    /** Give every node in the model a widget, and drop the widgets of nodes it
        no longer has. A bare VisualDirty only repaints what already has a pooled
        slot, so a node the panel just added stays invisible until this runs. */
    void RebuildNodeWidgets();
    /** Hold the pending wire on screen while the wire-drop menu is up: the
        gesture has ended, but the connection it is about to make has not. */
    void SetWireDropMenuOpen(bool open);
    /** When true, draw connections as straight lines; when false, use bezier curves (default). */
    void SetUseStraightLines(bool v) { m_UseStraightLines = v; MarkDirty(VisualDirty); }
    bool GetUseStraightLines() const { return m_UseStraightLines; }

    /** When true, draw grid and snap node positions / straight-line segments to grid. */
    void SetShowGrid(bool v) { m_ShowGrid = v; MarkDirty(VisualDirty); }
    bool GetShowGrid() const { return m_ShowGrid; }

    /** Live search filter: nodes whose typeId or display name match (case-insensitive) are highlighted. */
    void SetNodeSearchFilter(const std::string& filter);
    const std::string& GetNodeSearchFilter() const { return m_NodeSearchFilter; }
    void SetNodeSearchField(const std::string& field);

    /** Set currently active runtime nodes (highlighted separately from editor selection). */
    void SetRuntimeActiveNodes(const std::unordered_set<std::string>& nodeIds);

    /** Nodes the last compile blamed; they render with the error class until the
     *  next call. Pass an empty set to clear. */
    void SetErrorNodes(const std::unordered_set<std::string>& nodeIds);
    /** Add transition animation pulses for the provided link ids. */
    void AddRuntimeTransitionPulses(const std::vector<std::string>& linkIds);
    /** Advance active runtime transition pulse animation state. */
    void TickRuntimeTransitionAnimation(float deltaSeconds);
    /** Clear runtime node/link visualization state. */
    void ClearRuntimeVisualization();

    /** Remove selection/hover/drag state for nodes or links that no longer exist in the model (e.g. after undo). */
    void ReconcileStateWithModel();
    void FindNearestNonOverlappingPosition(const Graph::Node& node, float desiredX, float desiredY,
                                           const std::unordered_set<std::string>& ignoreNodeIds,
                                           float& outX, float& outY) const;
    void ResolveNodeOverlaps(const std::vector<std::string>& nodeIds);
    /** The overlap-free invariant, applied to the whole model: used at model
        load (positions saved under old metrics can collide) and programmatic
        graph builds. */
    void ResolveAllNodeOverlaps();

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

    /** Acquire `count` pooled GraphPortedNode slots on a grid. Spike ids use a `spike-` prefix. */
    void SpikeSpawnDummyNodes(int count);

private:
    void RegisterEventHandlers();
    void UnregisterEventHandlers();
    void EnsureNodeChrome();
    void MaybeArmSpikeDummyNodes();
    void SyncNodeLayerTransform();
    /* Rounds the pan to the device-pixel grid; see the definition for why.
       Canvas-internal bookkeeping: callers set pan through SetPanZoom. */
    void SnapPanToDevicePixels();
    void ApplyNodeLayerZoomClass();
    /** Put the node-title alignment class on the node layer. */
    void ApplyNodeTitleAlignment();
    void LayoutSpikeDummyNodes();

    /** Re-apply selected/search/runtime classes to bound node widgets without
        rebinding them (selection changes touch chrome only). */
    void ApplyNodeVisualStates();
    /** Push the drop-shadow preference onto every pooled node widget. */
    void ApplyNodeDropShadows();
    /** Populate the edit-host callbacks BindModel hands each node widget. */
    void FillNodeEditHost(GraphNodeEditHost& host);
    /** Apply a drag step to the model without recording it. */
    void ApplyLiveEdit(const std::string& nodeId, const std::function<void(Graph::Node&)>& write);
    /** Put back the state the drag started from, silently. */
    void RestoreStateBeforeLiveEdit();
    /** Bind (or acquire and bind) the pooled widget for one live model node;
        returns true when its rect changed. */
    bool BindLiveNode(const Graph::Node& node);
    /** Rebind the pooled widgets for the given live node ids (connected-port
        visuals change when a neighbour's links die). */
    /** Queue a model mutation to run outside input dispatch (FIFO). Private:
        the canvas defers its own mutations; the host only drives the flush. */
    void DeferModelMutation(std::function<void()> action);
    void HideAndDeferDeleteLink(const std::string& linkId, const char* undoName);
    void RebindNodes(const std::unordered_set<std::string>& nodeIds);
    /** Re-run BindModel on every node that already has a pooled slot, so the
        per-node style variables (node body, value surface, plate) pick up a
        settings change without reopening the graph. */
    void RebindVisibleNodes();
    /** Recycle the pooled widgets of the selected nodes without touching the
        model — the visual half of a hide-first delete. */
    void HideSelectedNodeWidgets();
    /** True while the link (or either endpoint node) is hidden pending a
        deferred delete: paint, hit tests and validity checks skip it. */
    bool IsLinkHiddenPendingDelete(const Graph::Edge& link) const;
    void RaiseSelectedNodes();
    void NotifyGraphChanged();
    bool NodeMatchesSearch(const Graph::Node& node) const;
    void GetPortCenterInGraph(const Graph::Node& node, size_t portIndex, float& outX, float& outY) const;
    void ScreenToGraph(float screenX, float screenY, float canvasX, float canvasY, float canvasW, float canvasH,
                       float& outGraphX, float& outGraphY) const;
    void GraphToScreen(float graphX, float graphY, float canvasX, float canvasY,
                      float& outScreenX, float& outScreenY) const;
    bool HitTestNodeOrPort(float graphX, float graphY, const Graph::Node& node,
                         std::string& outPortId, bool& outIsOutput) const;
    void FindHit(float graphX, float graphY, std::string& outNodeId, std::string& outPortId, bool& outIsOutput) const;
    /** If (screenX, screenY) is near a link curve, return that link id (empty otherwise). */
    std::string FindHitLink(float screenX, float screenY, float canvasX, float canvasY) const;
    bool FindHitLinkEndpoint(float screenX, float screenY, float canvasX, float canvasY,
                             std::string& outLinkId, bool& outNearSource) const;
    static const Graph::Port* FindPort(const Graph::Node& node, const std::string& portId);
    std::string BuildLinkTooltip(const Graph::Edge& link) const;
    /** True when dragging a link and (node, port) is a valid input target for the current source port. */
    /** Light every port the pending connection could legally land on, and mark
        the canvas 'connecting' so the stylesheet can dim the rest. */
    /** Double-click on a free input port: connect the nearest legal output to it. */
    void AutoConnectInputPort(const std::string& nodeId, const std::string& portId, float gx, float gy);
    void UpdateCompatiblePortHighlights();
    /** Run UpdateCompatiblePortHighlights when the drag state actually moved. */
    void RefreshCompatiblePortHighlights();
    /** The connection the user is dragging: one end held on a port, the other
        following the pointer.

        Whether a port may take the loose end is a question about this drag —
        what it started from, which way it runs, what it may replace — not a
        property the port carries. Ask the drag; nothing is stamped on ports. */
    struct ConnectionDrag
    {
        const Graph::Model* Model = nullptr;
        /// The end the user grabbed.
        std::string HeldNodeId;
        std::string HeldPortId;
        /// True when the held end is an input, so the loose end seeks an output.
        bool HeldOnInput = false;
        /// Links that do not count — pending deletion, or the one being rerouted.
        std::function<bool(const Graph::Edge&)> Ignores;

        /// True when the loose end may land on this port.
        bool Accepts(const Graph::Node& node, const Graph::Port& port) const;
    };
    /** The drag in progress. Empty when nothing is being dragged. */
    std::optional<ConnectionDrag> PendingConnection() const;
    bool WouldNodeOverlapAny(const Graph::Node& node, float x, float y,
                             const std::unordered_set<std::string>& ignoreNodeIds) const;
    /** Group connection links that visually overlap (parallel runs or a shared target
        port column) and assign each a track index so straight-line elbow risers fan
        apart instead of collapsing onto a single line. Inputs are per-link screen-space
        endpoints in m_Model->Links order. */
    void ComputeConnectionTracks(std::vector<float> segSx0, std::vector<float> segSy0,
                                 std::vector<float> segSx1, std::vector<float> segSy1,
                                 float canvasX, float canvasY,
                                 std::vector<int>& outTrack, std::vector<int>& outNTracks) const;
    /** Build the orthogonal polyline for a straight-mode connection. The riser is
        routed through the open gap between the two nodes for forward links, or
        around (above/below) both node bodies for backward/overlapping links, so the
        wire never crosses the nodes it connects. Points are emitted in screen space
        (pre-content-scale), source port first. */
    /** Every link's route, nudged apart where they share a run, in screen
     *  coordinates and indexed by link. Skipped links get an empty route.
     *  Drawing and hit-testing both ask for this in the same frame, so the
     *  result is cached against the graph and the view that shaped it. */
    void BuildStraightRoutes(float canvasX, float canvasY,
                             std::vector<GraphRouting::Route>& outRoutes) const;
    void ComputeStraightRoute(const Graph::Node& srcNode, const Graph::Node& tgtNode,
                              float sx0, float sy0, float sx1, float sy1,
                              float canvasX, float canvasY,
                              int trackIdx, int trackCount, float trackOffsetPx,
                              float srcPortBias, float tgtPortBias,
                              std::vector<Mathematics::Vector2>& outPoints) const;

    Graph::Model* m_Model = nullptr;
    float m_PanX = 0.f;
    float m_PanY = 0.f;
    float m_Zoom = 1.f;
    std::unordered_set<std::string> m_SelectedNodeIds;
    /** When dragging, start positions for each selected node (graph space). */
    std::unordered_map<std::string, Mathematics::Vector2> m_DragNodeStarts;
    std::string m_CopyPasteClipboard; /* JSON for copy/paste */
    bool m_DraggingPan = false;
    bool m_DraggingNode = false;
    bool m_DraggingLink = false;
    bool m_DraggingSelectionBox = false; /* true when dragging selection box (marquee) */
    std::string m_PendingLinkRemoveId; /* right-click deletes only if released without drag */
    bool m_RightMouseDragged = false;
    /* Selection captured at marquee mouse-down so Shift / Cmd / Ctrl gestures
       can compose with the prior selection during the live preview and at
       mouse-up, instead of wiping it as soon as the box covers a node. */
    std::unordered_set<std::string> m_MarqueeBaseSelection;
    /* Compose mode latched at mouse-down so the gesture stays consistent
       even if the user releases the modifier mid-drag.
         0 = replace (no modifier)
         1 = add hits to base (Shift)
         2 = remove hits from base (Cmd/Ctrl) */
    int m_MarqueeComposeMode = 0;
    float m_DragStartX = 0.f;
    float m_DragStartY = 0.f;
    float m_RightMouseDownX = 0.f;
    float m_RightMouseDownY = 0.f;
    /* Path length walked since the right/middle press, not the distance from
       it: a gesture that wanders and returns is still a drag, and the release
       must not open the menu. Reset at each press. */
    float m_RightMouseTravelPx = 0.f;
    float m_RightMouseLastX = 0.f;
    float m_RightMouseLastY = 0.f;
    float m_SelectionBoxStartX = 0.f; /* screen space start of selection box */
    float m_SelectionBoxStartY = 0.f;
    float m_SelectionBoxCurrentX = 0.f; /* screen space current position of selection box */
    float m_SelectionBoxCurrentY = 0.f;
    /** Node id when left-down on a node but drag not yet started (waiting for move threshold). */
    std::string m_PendingNodeDragId;
    std::string m_ConnectionSourceNodeId;
    std::string m_ConnectionSourcePortId;
    std::string m_ConnectionTargetNodeId;
    std::string m_ConnectionTargetPortId;
    float m_ConnectionEndGraphX = 0.f;
    float m_ConnectionEndGraphY = 0.f;
    bool m_DraggingLinkToExistingInput = false;
    std::string m_ReroutingLinkId;
    /** Double-click on output port: last port and time for auto-connect to green target */
    PressTracker m_LastPress;
    bool m_PendingNodeDoubleClick = false;
    std::string m_HoveredLinkId; /* link under mouse for highlight and click-to-disconnect */
    std::string m_SelectedLinkId; /* left-click selected wire */
    std::string m_HoveredNodeId; /* node whose port is under mouse */
    std::string m_HoveredPortId;  /* port under mouse for hover effect */
    bool m_WireDropMenuOpen = false;
    bool m_HighlightDragging = false;
    bool m_HighlightToExistingInput = false;
    std::string m_HighlightSourceNodeId;
    std::string m_HighlightSourcePortId;
    mutable uint64_t m_RouteCacheKey = 0;
    mutable std::vector<GraphRouting::Route> m_RouteCache;
    bool m_UseStraightLines = false; /* when true, draw connections as straight lines instead of bezier */
    bool m_ShowGrid = false;         /* when true, draw grid and snap nodes/lines to grid */
    std::string m_NodeSearchFilter;  /* when non-empty, nodes matching this (typeId/displayName) get highlight border */
    std::string m_NodeSearchField{"all"};
    std::unordered_set<std::string> m_RuntimeActiveNodeIds;
    std::unordered_set<std::string> m_ErrorNodeIds;
    struct RuntimeTransitionPulse
    {
        float Phase = 0.f;
        float Strength = 0.f;
    };
    std::unordered_map<std::string, RuntimeTransitionPulse> m_RuntimeTransitionPulses;

    OnSelectionChangedFn m_OnSelectionChanged;
    OnLinkSelectedFn m_OnLinkSelected;
    OnRequestContextMenuFn m_OnRequestContextMenu;
    OnRequestWireDropMenuFn m_OnRequestWireDropMenu;
    OnGraphChangedFn m_OnGraphChanged;
    UndoScopeFn m_UndoScope;
    /** A drag writes straight into the model, so by the time it ends the model
        already holds the finished value and an undo scope would record
        before == after. These hold the state the drag started from, restored
        just before the scope opens so it records the whole drag once. */
    std::string m_LiveEditNodeId;
    Graph::GraphObject m_LiveEditParams;
    bool m_HasLiveEdit = false;
    std::function<void()> m_OnNodeDragStarted;
    std::function<void()> m_OnNodeDragEnded;
    std::function<void(const std::string& nodeId)> m_OnNodeDoubleClicked;
    std::function<void()> m_OnPrimaryPress;

    struct EventTokens;
    std::unique_ptr<EventTokens> m_Tokens;

    UIElement* m_NodeLayer = nullptr;
    GraphOverlay* m_Overlay = nullptr;
    std::unique_ptr<GraphNodePool> m_NodePool;
    /** Expanded node view; see SetExpandedNodes. */
    bool m_ExpandedNodes = false;
    /** Click-to-pick services for texture slots; see SetAssetServices. */
    AssetRegistry* m_AssetRegistry = nullptr;
    IThumbnailProvider* m_ThumbnailProvider = nullptr;
    int m_SpikeDummyCount = 0;
    int m_NodeLayerZoomTier = -1;
    /** Index into the node-title alignment classes; -1 = none applied yet. */
    int m_NodeTitleAlignIndex = -1;
    /** Last zoom / corner radius published to the stylesheet as custom vars
        (--length-scale and the node/value/ring radii); republished only on
        change so pan frames don't re-cascade the node layer. */
    float m_PublishedZoom = -1.f;
    float m_PublishedCornerRadius = -1.f;
    float m_LaidOutZoom = -1.f;
    /** Re-entrancy guard: binding widgets can fire callbacks that call back
        into RebuildNodeWidgets mid-loop. */
    bool m_SyncingModelNodes = false;
    /* Hide-first delete: ids whose widgets and wires are already suppressed
       this frame while the matching model mutation waits in the deferred queue.
       Paint, hit tests and link validity all skip them, so the node is gone the
       moment it is deleted even though the model still lists it until the flush
       runs outside dispatch. Cleared when that mutation lands. */
    std::unordered_set<std::string> m_PendingHiddenNodeIds;
    std::unordered_set<std::string> m_PendingHiddenLinkIds;
    /* FIFO: a second mutation deferred inside the flush window must not
       overwrite the first, or its pending-hidden ids leak forever. */
    std::vector<std::function<void()>> m_DeferredModelMutations;
    bool m_DeferredModelMutationSkipOnce = false;
};

} // namespace GameEngine

#pragma once

// What one graph KIND contributes to the shared graph panel.
//
// The panel used to declare a virtual for each of these, so every kind's
// vocabulary sat in the generic base. A kind fills the entries it needs here
// instead, and the panel calls one only when a kind filled it. Adding a kind
// stops being an edit to the shared class.
//
// Keep this small. An entry earns its place by being a decision only the kind
// can make; anything a kind states once is plain data, and anything the panel
// only announces is an event on GraphPanel::Listen. Prefer one entry per moment
// over one entry per call: the panel decides WHEN, the kind decides WHAT.
//
// Every entry is optional. An empty entry means "the panel's own default",
// which the panel applies at the call site.

#include "Graph/GraphCanvas.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNest.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine {

class UIElement;

/** Where in the panel a chrome contribution goes. Toolbar icons sit with the
 *  shared ones, before the node-preview toggle that ends the icon run; anything
 *  that is not an icon goes in Toolbar, after it. */
enum class GraphPanelRegion
{
    StatusLabel,
    /** Full width between the toolbar and the body: a banner the kind raises
     *  over the canvas. */
    BelowToolbar,
    CanvasOverlays,
    Body,
};

/** One entry in the graph toolbar. A kind says what it wants and where it
    belongs; the panel decides the order and does the building, so no kind
    reaches into the toolbar element itself. */
struct GraphToolbarEntry
{
    /** Icons run together and end with the node-preview toggle; actions
        follow them. */
    enum class Place
    {
        Icon,
        Action
    };
    Place Where = Place::Icon;
    std::string Id;
    /** CSS classes: the glyph for an icon button, plus whatever the kind's
        stylesheet keys on. Empty for a plain text button. */
    std::vector<std::string> Classes;
    std::string Text;
    std::string Tooltip;
    std::function<void()> Activate;
    /** For an entry a button cannot express — the compile button and its
        status dot. Takes precedence over the fields above. */
    std::function<std::unique_ptr<UIElement>()> Build;
};

/** The kind's sample graph, and the palette row that loads it. An empty Label
 *  means the kind has no sample. */
struct GraphKindSample
{
    std::string Label;
    std::string SearchKey;
    std::function<std::filesystem::path(const std::filesystem::path& assetsRoot)> AssetPath;
    std::function<bool(Graph::Model& model)> Build;
};

/** Nested canvases (blend spaces, subgraphs). Only a kind that nests fills it. */
struct GraphKindNest
{
    /** Write live nest payload into `snapshotHost`. `liveHost` is the in-memory
     *  node, and is the same object when writing the live model. */
    std::function<void(Graph::Node& snapshotHost, const Graph::Node* liveHost, GraphNestKind kind,
                       bool liveTop)>
        WriteLiveHost;
    /** The visible nest changed. `host` is null at the top level, and when the
     *  panel left a nest. */
    std::function<void(const Graph::Node* host, GraphNestKind kind)> OnChanged;
};

struct GraphKindHooks
{
    /** Build the kind's own chrome into `host`. The panel calls this once per
     *  region it owns. The toolbar is not one of them: a kind declares its
     *  toolbar rather than building into it. */
    std::function<void(GraphPanelRegion region, UIElement& host)> ContributeChrome;

    /** What this kind puts in the toolbar, in its own order within each place. */
    std::function<std::vector<GraphToolbarEntry>()> ToolbarEntries;

    /** The panel's colour picker moved to another node. Kinds with colours
     *  route their own node to it. */
    std::function<void()> OnColorPickerChanged;

    /** Register the kind's node types and return the factory for them. The
     *  panel calls this once and keeps the factory. */
    std::function<GraphCanvas::NodeFactoryFn()> NodeCatalog;

    GraphKindSample Sample;
    GraphKindNest Nest;

    std::function<void(Graph::Model& model)> OnBeforeSave;
    std::function<std::filesystem::path(const std::filesystem::path& assetsRoot)>
        DefaultSaveDirectory;

    std::function<void(float dt)> OnUpdate;

    /** Rebuild the kind's own view of the graph's variables. */
    std::function<void()> SyncGraphVariables;
    /** A public variable changed. `syncDisk` asks the kind to persist. */
    std::function<void(bool syncDisk)> OnPublicVariablesEdited;
    /** A node's parameters changed. Returns true if that changed the variables,
     *  which makes the panel rebuild its variables list. */
    std::function<bool(const Graph::Node& node)> OnNodeParamsEdited;
    std::function<void(std::vector<Graph::Variable>& variables,
                       std::unordered_set<std::string>& names)>
        CollectImpliedVariables;

    bool ShowsGlobalVariableToggle = true;
};

} // namespace GameEngine

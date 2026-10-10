#pragma once

#include "InspectorRegistry.h"

#include "Graph/GraphModel.h"
#include "Graph/GraphNode.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {

class AssetRegistry;
class Checkbox;
class Dropdown;
class FloatField;
class GraphPort;
class GraphPortDropSlot;
class IThumbnailProvider;
class Label;
class TextField;
class Vector3Field;

struct GraphNodeEditHost {
    Graph::Model* Model = nullptr;
    /** Record an edit: one undo entry, taken on release. */
    std::function<void(const std::string& name, std::function<void()> mutate)> Undo;
    /** A value the user is still dragging. Apply it to the model now so the
        graph redraws under the pointer, and do NOT record it — Undo above
        records the whole drag once, when the drag ends. Keeping the two apart
        is the host's job: it is the one that knows what a single entry should
        contain. */
    std::function<void(const std::string& nodeId, const std::function<void(Graph::Node&)>& write)>
        EditLive;
    std::function<void()> OnChanged;
    std::function<bool(const std::string& nodeId, const std::string& portId)> IsInputConnected;
    /** Expanded node view: parameters with no inline port editor get a value row
        under the port area. Owned by the canvas; see GraphCanvas::SetExpandedNodes. */
    bool ExpandedView = false;
    /** Texture value nodes: click-to-pick services (search dialog provider).
        Null outside a panel with an asset context. */
    AssetRegistry* Assets = nullptr;
    IThumbnailProvider* Thumbnails = nullptr;
};

/** Ported node: C++ shareable root, inner body from graph-ported-node.uxml. */
class GraphPortedNode : public GraphNode {
public:
    GraphPortedNode();
    ~GraphPortedNode() override = default;

    void BindModel(const Graph::Node& node, std::string_view kindId, const GraphNodeVisualState& visual,
                   const GraphNodeEditHost* editHost = nullptr);
    void EnsurePortCount(int inputCount, int outputCount);
    void SetTitleText(const std::string& text);
    void SetValueText(const std::string& text);
    /** Border-only overlay, last child: hover/selection line painting above
        header and fields (the engine emits an element's outline before its
        children, so a node-level outline would sit under them). */
    UIElement* GetChromeRing() const { return m_Ring; }
    /** Whether this node draws value editors on its input port rows. Kinds
        that edit values elsewhere (the material graph's inspector-driven
        nodes) override to false; metrics and binding both consult this. */
    virtual bool DrawsInlinePortEditors(const Graph::Node& node) const
    {
        (void)node;
        return true;
    }
    /** Whether this node edits its port-less parameters through the in-flow
        central host (BindValueEditors). Row math consults it: the central
        host only fits a single-row body, so on a taller node those editors
        move into detail rows below the port area. Lives here, beside the
        editors it describes, so the predicate and BindValueEditors cannot
        drift; canvas geometry asks the pool's prototype. */
    virtual bool HasCentralEditors(const Graph::Node& node) const;
    /** Extra height (graph units) this node type reserves below its detail
        rows — the material graph's square preview plate. Zero for the base
        node. Canvas geometry asks the pool's prototype, so the answer must
        depend only on the node model and the view state. */
    virtual float ReservedBlockHeight(const Graph::Node& node, bool expandedView) const
    {
        (void)node;
        (void)expandedView;
        return 0.f;
    }
    /** Synthetic detail rows this node type appends after the parameter rows —
        the material parameter node's Value row. Zero for the base node. Canvas
        geometry asks the pool's prototype, so the answer must depend only on
        the node model and the view state. */
    virtual int SyntheticDetailRowCount(const Graph::Node& node, bool expandedView) const
    {
        (void)node;
        (void)expandedView;
        return 0;
    }
    void Reset() override;

protected:
    /** Write this node's parameters through the panel's undo scope. Every kind
        of node edits parameters; only the node itself does the writing. */
    void CommitParams(const std::function<void(Graph::Node&)>& write);

    /** Fills the node body's square slot during BindModel. The base node has no
        content for it and hides it; kinds that do (the material graph, whose
        nodes put a texture or a rendered preview there) override. Called only
        when the inner tree actually has the slot. */
    virtual void BindDetailSlot(const Graph::Node& node, GraphPortDropSlot& slot);

    /** Label text of synthetic detail row `extraIndex`. Separate from
        BindSyntheticDetailRow because the value column is measured before any
        row binds, and a synthetic label the measurement cannot see would
        overhang its own value box. Empty for the base node (count 0). */
    virtual std::string SyntheticDetailRowLabel(const Graph::Node& node, int extraIndex) const
    {
        (void)node;
        (void)extraIndex;
        return {};
    }

    /** Binds synthetic detail row `extraIndex` (0-based within the rows
        SyntheticDetailRowCount declared). The row scaffold — position, label
        text (SyntheticDetailRowLabel), editor host — is already prepared; the
        override fills the host's editor. Never called on the base node
        (count 0). */

    virtual void BindSyntheticDetailRow(const Graph::Node& node, int extraIndex, Label* label,
                                    UIElement* host)
    {
        (void)node;
        (void)extraIndex;
        (void)label;
        (void)host;
    }

    /** Parameter nodes edit their variable (Variables board entry), not node
        params. Resolved by variableName at mutate time so undo replays stay
        correct. */
    void CommitVariable(const std::function<void(Graph::Variable&)>& write);

    /** Pooled editor children for kind-specialized detail rows: reused when a
        matching child exists on the host, created and decorated otherwise. */
    static FloatField* EnsureRowFloatField(UIElement& host);
    static Vector3Field* EnsureRowVec3Field(UIElement& host);
    static TextField* EnsureRowTextField(UIElement& host);
    /** Whether the element (or a descendant) holds keyboard focus — a bind
        must not stomp the text the user is mid-editing. */
    static bool RowFieldHoldsFocus(UIElement* el);

    const GraphNodeEditHost& EditHost() const { return m_EditHost; }

    /** Adds an overlay child under the chrome ring. The ring must stay last:
        the engine emits an element's outline before its children, so anything
        after it would paint over the selection line. */
    void InsertBelowChrome(std::unique_ptr<UIElement> child);

    /** Rect height for this node under the host's view state, including any
        block ReservedBlockHeight reserves. Port lattice positions are a
        percentage of it, so it must be the height the element actually gets. */
    float RectHeightGraph(const Graph::Node& node) const;

    /** Top edge (graph units from the node top) of the block
        ReservedBlockHeight reserves — the bottom of the detail-row band.
        Node types position their block against this, never against the rect
        bottom: the rect carries grid-snap slack the rows do not, and anchoring
        to the bottom leaves that slack as a visible gap above the block. */
    float DetailBlockTopGraph(const Graph::Node& node) const;

    /** Left edge (graph units) of this node's one value column, with the
        node type's synthetic labels folded in. Inline port editors and detail
        rows both place their hosts here — measuring the column twice is how
        the two land on different edges. */
    float EditorStartGraph(const Graph::Node& node, bool drawsInlineEditors) const;

public:
    size_t InputPortCount() const { return m_InputPorts.size(); }
    size_t OutputPortCount() const { return m_OutputPorts.size(); }
    GraphPort* InputPortAt(size_t index) const;
    GraphPort* OutputPortAt(size_t index) const;
    GraphPort* FindPort(const std::string& portId) const;

private:
    /** Value rows under the port area for parameters with no inline port editor.
        No-ops when the host is not in expanded view and the node is not a
        multi-row central-editor node. */
    void BindDetailRows(const Graph::Node& node, std::string_view kindId);
    UIElement* EnsureDetailRowSlot(size_t row);
    void HideDetailRows(size_t firstUnused);
    void BuildInnerTree();
    void BindSlots();
    void BindSlotsFrom(UIElement& el);
    void GrowPorts(std::vector<GraphPort*>& ports, int count, bool isInput);
    void BindModelPorts(const Graph::Node& node);
    void BindValueEditors(const Graph::Node& node, std::string_view kindId);
    void BindPortEditors(const Graph::Node& node, std::string_view kindId);
    void HideValueEditors(bool force = false);
    void EnsureValueEditorHost();
    void CommitParam(const std::string& key, Graph::GraphValue value);
    /** Hand a still-dragging value to the host, which decides what reaches undo. */
    void EditLive(const std::string& key, Graph::GraphValue value);
    void EditLive(const std::function<void(Graph::Node&)>& write);

    UIElement* m_Body = nullptr;
    UIElement* m_Header = nullptr;
    GraphPortDropSlot* m_Preview = nullptr;
    UIElement* m_Icon = nullptr;
    Label* m_Title = nullptr;
    Label* m_Value = nullptr;
    UIElement* m_ValueEditorHost = nullptr;
    UIElement* m_Ring = nullptr;
    Vector3Field* m_ValueVec3 = nullptr;
    Dropdown* m_FunctionDropdown = nullptr;
    TextField* m_ValueText = nullptr;
    std::vector<FloatField*> m_ValueFloats;
    std::vector<GraphPort*> m_InputPorts;
    std::vector<GraphPort*> m_OutputPorts;
    std::vector<UIElement*> m_DetailRows;
    GraphNodeEditHost m_EditHost;
    /** BindModel depth; commits no-op so widget seeding cannot re-enter the canvas pool. */
    int m_BindSuppressDepth = 0;
};

} // namespace GameEngine

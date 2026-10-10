#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine {

namespace Graph { struct Node; }

class GraphPortedNode;
class UIElement;

/** Grow-only per-subclass pool. Surplus slots are Display:None, never visibility-hidden. */
class GraphNodePool {
public:
    /** Builds the node element a new slot holds; graph kinds install a factory
        producing their GraphPortedNode subclass. Empty = plain GraphPortedNode. */
    using NodeFactoryFn = std::function<std::unique_ptr<GraphPortedNode>()>;

    GraphNodePool(UIElement* layer, UIElement* canvas);

    /** Install the factory new slots are built with. Existing slots were built
        by the previous factory (wrong type), so they are destroyed; the next
        Acquire/Sync rebuilds through the new factory. */
    void SetFactory(NodeFactoryFn factory);

    GraphPortedNode* Acquire();
    void Recycle(GraphPortedNode* node);
    void Sync(size_t visibleCount);
    GraphPortedNode* FindByModelId(const std::string& id) const;

    size_t SlotCount() const { return m_Slots.size(); }
    GraphPortedNode* SlotAt(size_t index) const;

    /** True when this kind draws value editors on its input port rows, false
        when it edits values elsewhere (the material graph's inspector-driven
        nodes). The kind's answer to GraphPortedNode::DrawsInlinePortEditors,
        available
        without a bound slot — canvas geometry needs it for nodes that have no
        widget yet (framing, wire routing, nodes scrolled out of the pool).
        Backed by one unparented instance built from the same factory, so the
        answer cannot drift from what the real slots do. */
    bool DrawsInlinePortEditors(const Graph::Node& node) const;

    /** Same prototype, for whether the node type edits port-less parameters in
        the central host (GraphPortedNode::HasCentralEditors). */
    bool HasCentralEditors(const Graph::Node& node) const;

    /** Same prototype, for the height a node type reserves below its detail
        rows (GraphPortedNode::ReservedBlockHeight). */
    float ReservedBlockHeight(const Graph::Node& node, bool expandedView) const;

    /** Same prototype, for the synthetic detail rows a node type appends
        (GraphPortedNode::SyntheticDetailRowCount). */
    int SyntheticDetailRowCount(const Graph::Node& node, bool expandedView) const;

private:
    /** Lazily built, unparented instance of the installed node type. */
    GraphPortedNode* Prototype() const;

    struct Slot {
        GraphPortedNode* Node = nullptr;
        bool InUse = false;
    };

    UIElement* m_Layer = nullptr;
    UIElement* m_Canvas = nullptr;
    NodeFactoryFn m_Factory;
    std::vector<Slot> m_Slots;
    /** Built on first query and dropped whenever the factory changes. */
    mutable std::unique_ptr<GraphPortedNode> m_Prototype;
};

} // namespace GameEngine

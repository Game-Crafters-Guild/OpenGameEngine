#include "Graph/GraphNodePool.h"

#include "Graph/GraphPortedNode.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <memory>

namespace GameEngine {

GraphNodePool::GraphNodePool(UIElement* layer, UIElement* canvas)
    : m_Layer(layer)
    , m_Canvas(canvas)
{
}

void GraphNodePool::SetFactory(NodeFactoryFn factory)
{
    for (Slot& slot : m_Slots)
    {
        if (slot.Node && m_Layer)
            m_Layer->RemoveChild(slot.Node);
    }
    m_Slots.clear();
    m_Factory = std::move(factory);
    // Built by the previous factory, so it would answer for the wrong type.
    m_Prototype.reset();
    if (m_Canvas)
        m_Canvas->MarkDirty(UIElement::VisualDirty);
}

GraphPortedNode* GraphNodePool::Prototype() const
{
    if (!m_Prototype)
        m_Prototype = m_Factory ? m_Factory() : std::make_unique<GraphPortedNode>();
    return m_Prototype.get();
}

bool GraphNodePool::DrawsInlinePortEditors(const Graph::Node& node) const
{
    const GraphPortedNode* prototype = Prototype();
    return prototype ? prototype->DrawsInlinePortEditors(node) : true;
}

int GraphNodePool::SyntheticDetailRowCount(const Graph::Node& node, bool expandedView) const
{
    const GraphPortedNode* prototype = Prototype();
    return prototype ? prototype->SyntheticDetailRowCount(node, expandedView) : 0;
}

bool GraphNodePool::HasCentralEditors(const Graph::Node& node) const
{
    const GraphPortedNode* prototype = Prototype();
    return prototype ? prototype->HasCentralEditors(node) : false;
}

float GraphNodePool::ReservedBlockHeight(const Graph::Node& node, bool expandedView) const
{
    const GraphPortedNode* prototype = Prototype();
    return prototype ? prototype->ReservedBlockHeight(node, expandedView) : 0.f;
}

GraphPortedNode* GraphNodePool::Acquire()
{
    for (Slot& slot : m_Slots)
    {
        if (slot.InUse || !slot.Node)
            continue;
        slot.InUse = true;
        slot.Node->Reset();
        UI::Layout::SetElementHidden(*slot.Node, false);
        return slot.Node;
    }

    if (!m_Layer)
        return nullptr;

    std::unique_ptr<GraphPortedNode> node =
        m_Factory ? m_Factory() : std::make_unique<GraphPortedNode>();
    if (!node)
        return nullptr;
    GraphPortedNode* raw = node.get();
    m_Layer->AddChild(std::move(node));
    m_Slots.push_back(Slot{raw, true});
    if (m_Canvas)
        m_Canvas->MarkDirty(UIElement::VisualDirty);
    return raw;
}

void GraphNodePool::Recycle(GraphPortedNode* node)
{
    if (!node)
        return;
    for (Slot& slot : m_Slots)
    {
        if (slot.Node != node)
            continue;
        slot.InUse = false;
        node->Reset();
        UI::Layout::SetElementHidden(*node, true);
        return;
    }
}

void GraphNodePool::Sync(size_t visibleCount)
{
    while (m_Slots.size() < visibleCount)
    {
        if (!Acquire())
            break;
    }

    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        Slot& slot = m_Slots[i];
        if (!slot.Node)
            continue;
        if (i < visibleCount)
        {
            slot.InUse = true;
            UI::Layout::SetElementHidden(*slot.Node, false);
        }
        else
        {
            slot.InUse = false;
            slot.Node->Reset();
            UI::Layout::SetElementHidden(*slot.Node, true);
        }
    }
}

GraphPortedNode* GraphNodePool::SlotAt(size_t index) const
{
    if (index >= m_Slots.size())
        return nullptr;
    return m_Slots[index].Node;
}

GraphPortedNode* GraphNodePool::FindByModelId(const std::string& id) const
{
    if (id.empty())
        return nullptr;
    for (const Slot& slot : m_Slots)
    {
        if (!slot.InUse || !slot.Node)
            continue;
        if (slot.Node->GetModelNodeId() == id)
            return slot.Node;
    }
    return nullptr;
}

} // namespace GameEngine

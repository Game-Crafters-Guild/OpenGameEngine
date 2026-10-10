#pragma once

#include "Mathematics/Rect.h"
#include "UI/UIElement.h"

#include <string>

namespace GameEngine {

/** The independent visual states a graph node can be in at once. Named fields
 *  rather than positional bools: every one of them is a plain on/off flag. */
struct GraphNodeVisualState
{
    bool Selected = false;
    bool SearchMatch = false;
    bool RuntimeActive = false;
    bool DropTarget = false;
    bool HasError = false;
};

/** Pooled graph-node root. Identity lives in m_ModelNodeId, never element id. */
class GraphNode : public UIElement {
public:
    GraphNode();
    ~GraphNode() override = default;

    void SetModelNodeId(std::string id) { m_ModelNodeId = std::move(id); }
    const std::string& GetModelNodeId() const { return m_ModelNodeId; }

    void ApplyVisualState(const GraphNodeVisualState& state);
    bool SetGraphRect(const Mathematics::Rect& rect);
    virtual void Reset();

private:
    std::string m_ModelNodeId;
};

} // namespace GameEngine

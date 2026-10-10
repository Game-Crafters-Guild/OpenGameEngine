#include "Graph/GraphNode.h"

#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"

#include <memory>

namespace GameEngine {

namespace {

void SetClassEnabled(UIElement& el, const char* className, bool enabled)
{
    if (enabled)
        el.AddClass(className);
    else
        el.RemoveClass(className);
}

} // namespace

GraphNode::GraphNode()
{
    AddClass("graph-node");
    SetFocusable(false);
}

void GraphNode::ApplyVisualState(const GraphNodeVisualState& state)
{
    SetClassEnabled(*this, "selected", state.Selected);
    SetClassEnabled(*this, "search-match", state.SearchMatch);
    /* Both states paint the same accent ring. Deciding that here means the
       stylesheet needs one selector instead of enumerating the states (and
       their :hover variants) on the ring's parent. */
    SetClassEnabled(*this, "ring-accent", state.Selected || state.SearchMatch);
    SetClassEnabled(*this, "runtime-active", state.RuntimeActive);
    SetClassEnabled(*this, "drop-target", state.DropTarget);
    SetClassEnabled(*this, "has-error", state.HasError);
}

bool GraphNode::SetGraphRect(const Mathematics::Rect& rect)
{
    /* Always the full layout path: children (ring, ports, value hosts) carry
       committed rects only a solve refreshes, so a visual-only move here
       leaves them stranded at the previous node's geometry. The drag path
       owns the position-only fast path (ApplyNodeWidgetRect), where it also
       offsets the committed child rects itself. */
    return UI::Layout::SetAbsolutePosition(*this, rect);
}

void GraphNode::Reset()
{
    m_ModelNodeId.clear();
    ApplyVisualState({});
}

} // namespace GameEngine

namespace RegisterGraphElements
{
static auto s_reg_graphNode =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::GraphNode>(
        "GraphNode",
        []() { return std::make_unique<GameEngine::GraphNode>(); })
        .TagAlias("graphnode");
}

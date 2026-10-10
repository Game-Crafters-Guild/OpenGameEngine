#include "Graph/GraphNest.h"

namespace GameEngine {

GraphNestKind NestKindForDoubleClick(GraphNestKind current, std::string_view hostTypeId)
{
    if (current == GraphNestKind::BlendSpace1D || current == GraphNestKind::BlendSpace2D)
        return current;

    switch (current)
    {
    case GraphNestKind::Root:
        if (hostTypeId == "StateMachine")
            return GraphNestKind::StateMachine;
        if (hostTypeId == "BlendSpace1D")
            return GraphNestKind::BlendSpace1D;
        if (hostTypeId == "BlendSpace2D")
            return GraphNestKind::BlendSpace2D;
        return GraphNestKind::Root;
    case GraphNestKind::StateMachine:
        if (hostTypeId == "State")
            return GraphNestKind::PoseGraph;
        return GraphNestKind::StateMachine;
    case GraphNestKind::PoseGraph:
        if (hostTypeId == "BlendSpace1D")
            return GraphNestKind::BlendSpace1D;
        if (hostTypeId == "BlendSpace2D")
            return GraphNestKind::BlendSpace2D;
        return GraphNestKind::PoseGraph;
    default:
        return current;
    }
}

const char* NestKindTitle(GraphNestKind kind)
{
    switch (kind)
    {
    case GraphNestKind::StateMachine:
        return "State Machine";
    case GraphNestKind::PoseGraph:
        return "State";
    case GraphNestKind::BlendSpace1D:
        return "Blend Space 1D";
    case GraphNestKind::BlendSpace2D:
        return "Blend Space 2D";
    case GraphNestKind::Root:
        return "Graph";
    }
    return "";
}

bool NestKindUsesSubgraphModel(GraphNestKind kind)
{
    return kind == GraphNestKind::StateMachine || kind == GraphNestKind::PoseGraph;
}

bool NestKindHidesCanvas(GraphNestKind kind)
{
    return kind == GraphNestKind::BlendSpace1D || kind == GraphNestKind::BlendSpace2D;
}

} // namespace GameEngine

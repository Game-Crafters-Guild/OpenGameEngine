#pragma once

#include "Graph/GraphModel.h"

#include <string>
#include <string_view>

namespace GameEngine {

enum class GraphNestKind
{
    Root,
    StateMachine,
    PoseGraph,
    BlendSpace1D,
    BlendSpace2D
};

struct GraphNestFrame
{
    std::string HostNodeId;
    GraphNestKind Kind = GraphNestKind::Root;
    Graph::Model Model; // used for StateMachine and PoseGraph; unused for BlendSpace1D
};

GraphNestKind NestKindForDoubleClick(GraphNestKind current, std::string_view hostTypeId);

const char* NestKindTitle(GraphNestKind kind);
bool NestKindUsesSubgraphModel(GraphNestKind kind);
bool NestKindHidesCanvas(GraphNestKind kind);

} // namespace GameEngine

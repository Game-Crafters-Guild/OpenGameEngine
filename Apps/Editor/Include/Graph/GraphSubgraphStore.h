#pragma once

#include "Graph/GraphModel.h"

namespace GameEngine {

/**
 * Nested Graph::Model stored on a host node as Extensions["subgraph"]
 * (JSON string from Graph::ToJson / Graph::FromJson).
 */
class GraphSubgraphStore
{
public:
    /** Missing, empty, or invalid JSON leaves `out` unchanged and returns false. */
    static bool TryLoadSubgraph(const Graph::Node& host, Graph::Model& out);
    static void StoreSubgraph(Graph::Node& host, const Graph::Model& nested);
};

} // namespace GameEngine

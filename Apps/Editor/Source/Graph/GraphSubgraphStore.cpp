#include "Graph/GraphSubgraphStore.h"

namespace GameEngine {

namespace {
constexpr const char* kSubgraphExtensionKey = "subgraph";
}

bool GraphSubgraphStore::TryLoadSubgraph(const Graph::Node& host, Graph::Model& out)
{
    const auto it = host.Extensions.find(kSubgraphExtensionKey);
    if (it == host.Extensions.end())
        return false;
    const std::string* json = it->second.TryString();
    if (!json || json->empty())
        return false;

    Graph::Model loaded;
    if (!Graph::FromJson(*json, loaded))
        return false;
    out = std::move(loaded);
    return true;
}

void GraphSubgraphStore::StoreSubgraph(Graph::Node& host, const Graph::Model& nested)
{
    host.Extensions[kSubgraphExtensionKey] = Graph::ToJson(nested);
}

} // namespace GameEngine

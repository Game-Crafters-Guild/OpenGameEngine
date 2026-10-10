#pragma once

#include "Graph/GraphModel.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {
namespace Graph {

/**
 * Descriptor for a graph kind. GameLogic and Material are registered by the
 * Graph module. Additional kinds (e.g. animation authoring) Register() from
 * their owning module — do not add a Graph::Kind enumerator for them.
 */
struct GraphTypeDesc
{
    std::string Id;
    std::string DisplayName;
    std::string FileExtension = ".graph";
};

/**
 * Process-wide registry of graph kinds. Pointers from Find remain valid for
 * the process lifetime (Register only appends).
 */
class GraphTypeRegistry
{
public:
    static GraphTypeRegistry& Get();

    /** First registration of an id wins. Empty ids are ignored. */
    void Register(GraphTypeDesc desc);
    const GraphTypeDesc* Find(std::string_view id) const;
    std::vector<GraphTypeDesc> All() const;

    static std::string_view IdFromKind(Kind kind);
    /** False when id is not one of the two builtin Kind enumerators. */
    static bool TryParseKind(std::string_view id, Kind& outKind);

private:
    GraphTypeRegistry();

    std::deque<GraphTypeDesc> m_Types;
};

} // namespace Graph
} // namespace GameEngine

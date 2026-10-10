#include "Graph/GraphTypeRegistry.h"

namespace GameEngine {
namespace Graph {

GraphTypeRegistry& GraphTypeRegistry::Get()
{
    static GraphTypeRegistry s_Instance;
    return s_Instance;
}

GraphTypeRegistry::GraphTypeRegistry()
{
    Register({std::string(kKindIdGameLogic), "Game Logic", ".graph"});
    Register({std::string(kKindIdMaterial), "Shader Graph", ".glsl"});
}

void GraphTypeRegistry::Register(GraphTypeDesc desc)
{
    if (desc.Id.empty() || Find(desc.Id) != nullptr)
        return;
    m_Types.push_back(std::move(desc));
}

const GraphTypeDesc* GraphTypeRegistry::Find(std::string_view id) const
{
    for (const GraphTypeDesc& type : m_Types)
    {
        if (type.Id == id)
            return &type;
    }
    return nullptr;
}

std::vector<GraphTypeDesc> GraphTypeRegistry::All() const
{
    return {m_Types.begin(), m_Types.end()};
}

std::string_view GraphTypeRegistry::IdFromKind(Kind kind)
{
    return kind == Kind::Material ? kKindIdMaterial : kKindIdGameLogic;
}

bool GraphTypeRegistry::TryParseKind(std::string_view id, Kind& outKind)
{
    if (id == kKindIdMaterial)
    {
        outKind = Kind::Material;
        return true;
    }
    if (id == kKindIdGameLogic)
    {
        outKind = Kind::GameLogic;
        return true;
    }
    return false;
}

} // namespace Graph
} // namespace GameEngine

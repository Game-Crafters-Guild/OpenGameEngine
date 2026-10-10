#include "ShaderGraph/MaterialGraphVariables.h"

namespace GameEngine {
namespace MaterialGraphVariables {

bool IsVariableNode(const Graph::Node& node)
{
    return node.TypeId == "FloatParameter" || node.TypeId == "Vec2Parameter" ||
           node.TypeId == "Vec3Parameter" || node.TypeId == "Vec4Parameter" ||
           node.TypeId == "ColorParameter";
}

std::string TypeFromNode(const Graph::Node& node)
{
    if (node.TypeId == "Vec2Parameter")
        return "float2";
    // A colour is three floats here: the picker edits it, the wire format does
    // not distinguish it from a vector.
    if (node.TypeId == "Vec3Parameter" || node.TypeId == "ColorParameter")
        return "float3";
    if (node.TypeId == "Vec4Parameter")
        return "float4";
    return "float";
}

std::string DefaultValueForType(const std::string& type)
{
    if (type == "float2")
        return "0, 0";
    if (type == "float3")
        return "0, 0, 0";
    if (type == "float4")
        return "0, 0, 0, 0";
    return "0";
}

} // namespace MaterialGraphVariables
} // namespace GameEngine

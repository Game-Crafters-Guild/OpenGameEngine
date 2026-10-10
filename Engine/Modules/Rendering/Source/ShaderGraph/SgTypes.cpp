#include "Rendering/ShaderGraph/SgTypes.h"

#include <algorithm>

namespace GameEngine::ShaderGraph
{
namespace
{

bool IsVectorGraphType(std::string_view t)
{
    return t == "float2" || t == "float3" || t == "float4";
}

} // namespace

std::string GlslTypeToGraphType(std::string_view glslType)
{
    if (glslType == "float")
        return "float";
    if (glslType == "vec2")
        return "float2";
    if (glslType == "vec3")
        return "float3";
    if (glslType == "vec4")
        return "float4";
    if (glslType == "int" || glslType == "uint")
        return "int";
    if (glslType == "bool")
        return "bool";
    if (glslType == "sampler2D")
        return "texture2d";
    if (glslType == "sampler2DArray")
        return "texture2d_array";
    if (glslType == "samplerCube")
        return "texture_cube";
    return std::string(glslType);
}

std::string GraphTypeToGlslType(std::string_view graphType)
{
    if (graphType == "float")
        return "float";
    if (graphType == "float2")
        return "vec2";
    if (graphType == "float3")
        return "vec3";
    if (graphType == "float4")
        return "vec4";
    if (graphType == "int")
        return "int";
    if (graphType == "bool")
        return "bool";
    if (graphType == "texture2d")
        return "sampler2D";
    if (graphType == "texture2d_array")
        return "sampler2DArray";
    if (graphType == "texture_cube")
        return "samplerCube";
    return std::string(graphType);
}

bool ArePortTypesCompatible(std::string_view sourceGraphType, std::string_view targetGraphType)
{
    if (sourceGraphType.empty() || targetGraphType.empty() || sourceGraphType == "any" ||
        targetGraphType == "any")
        return true;
    if (sourceGraphType == targetGraphType)
        return true;
    if (sourceGraphType == "float" && IsVectorGraphType(targetGraphType))
        return true;
    if (sourceGraphType == "int" && targetGraphType == "float")
        return true;
    return false;
}

std::string BroadcastScalarToVector(std::string_view scalarExpr, std::string_view targetGlslType)
{
    if (targetGlslType == "vec2")
        return "vec2(" + std::string(scalarExpr) + ")";
    if (targetGlslType == "vec3")
        return "vec3(" + std::string(scalarExpr) + ")";
    if (targetGlslType == "vec4")
        return "vec4(" + std::string(scalarExpr) + ")";
    return std::string(scalarExpr);
}

} // namespace GameEngine::ShaderGraph

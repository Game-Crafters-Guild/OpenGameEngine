#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::ShaderGraph
{

enum class SgShaderStage
{
    Fragment,
    Vertex,
    Any
};

enum class SgGraphStage
{
    Surface,
    Vertex,
    Both
};

struct SgPortMeta
{
    std::string Name;
    std::string GlslType;
    std::string GraphType;
    std::string DisplayName;
    std::string DefaultExpression;
    std::string RangeMin;
    std::string RangeMax;
    std::string Hint;
    bool Hidden = false;
};

struct SgNodeOverload
{
    std::string FunctionName;
    std::vector<SgPortMeta> Inputs;
    std::vector<SgPortMeta> Outputs;
    bool Pure = false;
    SgShaderStage Stage = SgShaderStage::Any;
};

struct SgNodeDefinition
{
    std::string TypeId;
    std::string DisplayName;
    std::string Category;
    std::string Tooltip;
    std::string Keywords;
    std::string DeprecatedReason;
    std::string SourceFile;
    std::vector<SgNodeOverload> Overloads;
};

struct SgNodeLibraryIndex
{
    std::unordered_map<std::string, SgNodeDefinition> NodesByType;
    std::vector<std::string> IncludePaths;
};

struct SgGraphProperty
{
    std::string Name;
    std::string Type;
    std::string DefaultValue;
    std::string RangeMin;
    std::string RangeMax;
    std::string Hint;
    bool IsPublic = false;
};

struct SgGraphTexture
{
    std::string Name;
    std::string Guid;
    std::string Hint;
};

struct SgGraphNode
{
    std::string Id;
    std::string TypeId;
    float PositionX = 0.f;
    float PositionY = 0.f;
    std::unordered_map<std::string, std::string> PortDefaults;
};

struct SgGraphEdge
{
    std::string SourceNodeId;
    std::string SourcePortId;
    std::string TargetNodeId;
    std::string TargetPortId;
};

struct SgGraphDocument
{
    std::string GraphName;
    int Version = 1;
    SgGraphStage Stage = SgGraphStage::Surface;
    std::string LightingModel = "StandardPBR";
    std::vector<std::string> VariantDefines;
    std::vector<SgGraphProperty> Properties;
    std::vector<SgGraphTexture> Textures;
    std::vector<SgGraphNode> Nodes;
    std::vector<SgGraphEdge> Edges;
    // Tag lines the parser does not recognize, kept verbatim so a save never
    // drops data written by a newer or external tool.
    std::vector<std::string> UnknownTags;
    // Written on a materialized (built) surface: its tag block carries only the
    // semantic tags of the graph that produced it and no nodes, so it is never
    // a graph to compile again.
    bool Materialized = false;
    float ViewportPanX = 0.f;
    float ViewportPanY = 0.f;
    float ViewportZoom = 1.f;
};

/** One compile failure. NodeId names the offending node when the compiler knows
 *  which one it is, so an editor can point at it without re-parsing Message;
 *  it is empty for graph-scoped failures (a missing surface sink has no node). */
struct SgDiagnostic
{
    std::string Message;
    std::string NodeId;
};

struct SgCompileResult
{
    bool Success = false;
    std::string Body;
    std::vector<std::string> Includes;
    std::vector<std::string> VariantDefines;
    std::vector<SgGraphProperty> Properties;
    std::vector<SgGraphTexture> Textures;
    std::vector<SgDiagnostic> Errors;
    std::vector<std::string> Warnings;
};

std::string GlslTypeToGraphType(std::string_view glslType);
std::string GraphTypeToGlslType(std::string_view graphType);
bool ArePortTypesCompatible(std::string_view sourceGraphType, std::string_view targetGraphType);
std::string BroadcastScalarToVector(std::string_view scalarExpr, std::string_view targetGlslType);

} // namespace GameEngine::ShaderGraph

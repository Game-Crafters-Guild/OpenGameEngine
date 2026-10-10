#include "Rendering/ShaderGraph/SgPseudoNodes.h"

namespace GameEngine::ShaderGraph
{
namespace
{

SgPortMeta MakePort(const char* name, const char* glsl, const char* graph, const char* display = nullptr)
{
    SgPortMeta port;
    port.Name = name;
    port.GlslType = glsl;
    port.GraphType = graph;
    port.DisplayName = display ? display : name;
    return port;
}

SgNodeDefinition MakeSourceNode(const char* typeId, const char* display, const char* category,
                                std::vector<SgPortMeta> outputs)
{
    SgNodeDefinition def;
    def.TypeId = typeId;
    def.DisplayName = display;
    def.Category = category;
    SgNodeOverload overload;
    overload.FunctionName = typeId;
    overload.Outputs = std::move(outputs);
    overload.Stage = SgShaderStage::Any;
    def.Overloads.push_back(std::move(overload));
    return def;
}

SgNodeDefinition MakeSinkNode(const char* typeId, const char* display, const char* category,
                              std::vector<SgPortMeta> inputs, SgShaderStage stage)
{
    SgNodeDefinition def;
    def.TypeId = typeId;
    def.DisplayName = display;
    def.Category = category;
    SgNodeOverload overload;
    overload.FunctionName = typeId;
    overload.Inputs = std::move(inputs);
    overload.Stage = stage;
    def.Overloads.push_back(std::move(overload));
    return def;
}

} // namespace

std::string ResolveNodeTypeAlias(const std::string& typeId)
{
    if (typeId == "SampleTexture")
        return "SampleTexture2D";
    if (typeId == "TriplanarTexture")
        return "TriplanarSample";
    return typeId;
}

bool IsPseudoNodeType(const std::string& typeId)
{
    const std::string t = ResolveNodeTypeAlias(typeId);
    return t == "EngineInput" || t == "SurfaceOutput" || t == "VertexOutput" || t == "PropertyRef" ||
           t == "TextureRef" || t == "Group" || t == "Comment";
}

void RegisterPseudoNodes(SgNodeLibraryIndex& index)
{
    {
        std::vector<SgPortMeta> outs;
        outs.push_back(MakePort("UV0", "vec2", "float2", "UV0"));
        outs.push_back(MakePort("UV1", "vec2", "float2", "UV1"));
        outs.push_back(MakePort("WorldPosition", "vec3", "float3", "World Position"));
        outs.push_back(MakePort("WorldNormal", "vec3", "float3", "World Normal"));
        outs.push_back(MakePort("WorldTangent", "vec3", "float3", "World Tangent"));
        outs.push_back(MakePort("ViewDirection", "vec3", "float3", "View Direction"));
        outs.push_back(MakePort("VertexColor", "vec4", "float4", "Vertex Color"));
        outs.push_back(MakePort("ScreenUV", "vec2", "float2", "Screen UV"));
        outs.push_back(MakePort("LinearDepth", "float", "float", "Linear Depth"));
        outs.push_back(MakePort("Time", "float", "float", "Time"));
        outs.push_back(MakePort("CameraPosition", "vec3", "float3", "Camera Position"));
        outs.push_back(MakePort("Position", "vec3", "float3", "Position"));
        outs.push_back(MakePort("Normal", "vec3", "float3", "Normal"));
        outs.push_back(MakePort("Tangent", "vec4", "float4", "Tangent"));
        auto def = MakeSourceNode("EngineInput", "Engine Input", "Input", std::move(outs));
        index.NodesByType.emplace(def.TypeId, std::move(def));
    }
    {
        std::vector<SgPortMeta> ins;
        ins.push_back(MakePort("BaseColor", "vec3", "float3", "Base Color"));
        ins.push_back(MakePort("Metallic", "float", "float", "Metallic"));
        ins.push_back(MakePort("Roughness", "float", "float", "Roughness"));
        ins.push_back(MakePort("Normal", "vec3", "float3", "Normal (TS)"));
        ins.push_back(MakePort("Emissive", "vec3", "float3", "Emissive"));
        ins.push_back(MakePort("Opacity", "float", "float", "Opacity"));
        ins.push_back(MakePort("AmbientOcclusion", "float", "float", "AO"));
        auto def = MakeSinkNode("SurfaceOutput", "Surface Output", "Output", std::move(ins),
                                SgShaderStage::Fragment);
        index.NodesByType.emplace(def.TypeId, std::move(def));
    }
    {
        std::vector<SgPortMeta> ins;
        ins.push_back(MakePort("PositionOffset", "vec3", "float3", "Position Offset"));
        auto def = MakeSinkNode("VertexOutput", "Vertex Output", "Output", std::move(ins),
                                SgShaderStage::Vertex);
        index.NodesByType.emplace(def.TypeId, std::move(def));
    }
}

const SgNodeDefinition* FindPseudoNode(const SgNodeLibraryIndex& index, const std::string& typeId)
{
    const auto it = index.NodesByType.find(ResolveNodeTypeAlias(typeId));
    return it != index.NodesByType.end() ? &it->second : nullptr;
}

} // namespace GameEngine::ShaderGraph

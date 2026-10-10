#include "Rendering/ShaderGraph/SgGraphCompiler.h"

#include "Rendering/ShaderGraph/SgPseudoNodes.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::ShaderGraph
{
namespace
{

std::string PortKey(const std::string& nodeId, const std::string& portId)
{
    return nodeId + "." + portId;
}

const SgGraphNode* FindNode(const SgGraphDocument& doc, const std::string& id)
{
    for (const auto& n : doc.Nodes)
    {
        if (n.Id == id)
            return &n;
    }
    return nullptr;
}

const SgGraphEdge* FindEdgeTo(const SgGraphDocument& doc, const std::string& nodeId, const std::string& portId)
{
    for (const auto& e : doc.Edges)
    {
        if (e.TargetNodeId == nodeId && e.TargetPortId == portId)
            return &e;
    }
    return nullptr;
}

const SgGraphEdge* FindEdgeFrom(const SgGraphDocument& doc, const std::string& nodeId, const std::string& portId)
{
    for (const auto& e : doc.Edges)
    {
        if (e.SourceNodeId == nodeId && e.SourcePortId == portId)
            return &e;
    }
    return nullptr;
}

/* Zero must match the port's width: a bare 0.0 spliced into a vector port is a
   GLSL type error the graph author never sees until SPIR-V compilation. */
std::string ZeroExpressionForType(const std::string& glslType)
{
    if (glslType == "vec2" || glslType == "vec3" || glslType == "vec4")
        return glslType + "(0.0)";
    return "0.0";
}

bool IsPropertyName(const SgGraphDocument& doc, const std::string& name)
{
    for (const auto& p : doc.Properties)
    {
        if (p.Name == name)
            return true;
    }
    return false;
}

bool IsTextureName(const SgGraphDocument& doc, const std::string& name)
{
    for (const auto& t : doc.Textures)
    {
        if (t.Name == name)
            return true;
    }
    return false;
}

const SgNodeOverload* PickOverload(const SgNodeDefinition& def, const std::vector<std::string>& argTypes)
{
    const SgNodeOverload* best = nullptr;
    size_t bestScore = 0;
    for (const auto& overload : def.Overloads)
    {
        if (overload.Inputs.size() != argTypes.size())
            continue;
        size_t score = 0;
        bool ok = true;
        for (size_t i = 0; i < argTypes.size(); ++i)
        {
            if (!ArePortTypesCompatible(argTypes[i], overload.Inputs[i].GraphType))
            {
                ok = false;
                break;
            }
            if (argTypes[i] == overload.Inputs[i].GraphType)
                ++score;
        }
        if (ok && score >= bestScore)
        {
            bestScore = score;
            best = &overload;
        }
    }
    if (best)
        return best;
    return def.Overloads.empty() ? nullptr : &def.Overloads.front();
}

std::string SanitizeTemp(const std::string& nodeId)
{
    std::string out = "_";
    for (char c : nodeId)
        out.push_back((c == '-' || c == '.') ? '_' : c);
    return out;
}

std::string ParameterSlotExpression(const SgGraphNode& node, std::string_view typeId)
{
    int slot = 0;
    if (auto slotIt = node.PortDefaults.find("slot"); slotIt != node.PortDefaults.end())
    {
        try
        {
            slot = std::stoi(slotIt->second);
        }
        catch (...)
        {
            slot = 0;
        }
    }

    std::string slotExpr = "Mat.uParams0";
    if (slot == 0)
        slotExpr = "Mat.uBaseColor";
    else if (slot == 1)
        slotExpr = "Mat.uParams0";
    else if (slot == 2)
        slotExpr = "Mat.uParams2";
    else if (slot == 3)
        slotExpr = "Mat.uParams3";

    auto swizzleIt = node.PortDefaults.find("swizzle");
    const std::string swizzle =
        (swizzleIt != node.PortDefaults.end() && !swizzleIt->second.empty()) ? swizzleIt->second : "";

    if (typeId == "FloatParameter")
    {
        const char* comp = "x";
        if (auto compIt = node.PortDefaults.find("component"); compIt != node.PortDefaults.end() &&
            !compIt->second.empty())
        {
            comp = compIt->second.c_str();
        }
        return slotExpr + "." + comp;
    }
    if (typeId == "Vec2Parameter")
        return slotExpr + "." + (swizzle.empty() ? "xy" : swizzle);
    if (typeId == "Vec3Parameter" || typeId == "ColorParameter")
        return slotExpr + "." + (swizzle.empty() ? "xyz" : swizzle);
    if (typeId == "Vec4Parameter")
        return slotExpr + "." + (swizzle.empty() ? "xyzw" : swizzle);
    return slotExpr;
}

struct CompileContext
{
    const SgGraphDocument& doc;
    const SgNodeLibraryIndex& library;
    SgCompileResult& result;
    std::string* nodeBody = nullptr;
    std::unordered_map<std::string, std::string> portExprs;
    std::unordered_set<std::string> visited;
    std::vector<std::string> variantDefines;
    bool wiringNormalTs = false;
};

/* The vertex stage reads attributes and UBOs; the interpolated surface inputs
   (`sIn`) only exist in the fragment stage. A port with no vertex expression
   returns nullopt and the caller reports it. */
std::optional<std::string> GetEngineInputExpr(const std::string& port, bool vertexStage)
{
    if (vertexStage)
    {
        if (port == "Position")
            return "position";
        if (port == "Normal")
            return "aNormal";
        if (port == "UV0")
            return "aUV0";
        if (port == "UV1")
            return "aUV1";
        if (port == "VertexColor")
            return "aColor";
        if (port == "Tangent")
            return "aTangent";
        // The vertex stage deforms at its endpoint's clock (instance_io.glsl), never the
        // per-view global, so the same ModifyVertex can run at a second endpoint.
        if (port == "Time")
            return "inst.deformationTimeSeconds";
        if (port == "CameraPosition")
            return "Cam.uCameraPos.xyz";
        return std::nullopt;
    }
    if (port == "UV0")
        return "sIn.uv0";
    if (port == "UV1")
        return "sIn.uv1";
    if (port == "WorldPosition")
        return "sIn.positionWS";
    if (port == "WorldNormal")
        return "sIn.normalWS";
    if (port == "WorldTangent")
        return "sIn.tangentWS.xyz";
    if (port == "ViewDirection")
        return "sIn.viewDirWS";
    if (port == "VertexColor")
        return "sIn.vertexColor";
    if (port == "ScreenUV")
        return "sIn.screenUV";
    if (port == "LinearDepth")
        return "sIn.linearDepth";
    if (port == "Time")
        return "Light.uTimeParams.x";
    if (port == "CameraPosition")
        return "Cam.uCameraPos.xyz";
    if (port == "Position")
        return "position";
    return "0.0";
}

const SgNodeDefinition* FindDefinition(const SgNodeLibraryIndex& library, const std::string& typeId)
{
    const std::string resolved = ResolveNodeTypeAlias(typeId);
    if (const auto* pseudo = FindPseudoNode(library, resolved))
        return pseudo;
    const auto it = library.NodesByType.find(resolved);
    return it != library.NodesByType.end() ? &it->second : nullptr;
}

std::string ResolveSourceExpr(CompileContext& ctx, const SgGraphEdge& edge, const std::string& targetGlslType,
                              bool vertexStage);

std::string ResolveNodeOutput(CompileContext& ctx, const std::string& nodeId, const std::string& portId,
                              const std::string& targetGlslType, bool vertexStage);

std::string ResolveSourceExpr(CompileContext& ctx, const SgGraphEdge& edge, const std::string& targetGlslType,
                              bool vertexStage)
{
    if (IsPropertyName(ctx.doc, edge.SourceNodeId))
        return "Mat." + edge.SourceNodeId;
    if (IsTextureName(ctx.doc, edge.SourceNodeId))
        return edge.SourceNodeId;
    return ResolveNodeOutput(ctx, edge.SourceNodeId, edge.SourcePortId, targetGlslType, vertexStage);
}

void CompileNodeInstance(CompileContext& ctx, const SgGraphNode& node, bool vertexStage);

std::string ResolveNodeOutput(CompileContext& ctx, const std::string& nodeId, const std::string& portId,
                              const std::string& targetGlslType, bool vertexStage)
{
    const auto key = PortKey(nodeId, portId);
    auto it = ctx.portExprs.find(key);
    if (it == ctx.portExprs.end())
    {
        const SgGraphNode* graphNode = FindNode(ctx.doc, nodeId);
        if (!graphNode)
        {
            ctx.result.Errors.push_back({"ShaderGraph: unresolved node '" + nodeId + "'.", nodeId});
            return "0.0";
        }

        CompileNodeInstance(ctx, *graphNode, vertexStage);
        it = ctx.portExprs.find(key);
        if (it == ctx.portExprs.end())
        {
            ctx.result.Errors.push_back(
                {"ShaderGraph: missing output port '" + portId + "' on node '" + nodeId + "'.", nodeId});
            return "0.0";
        }
    }

    // Width adaptation runs on every resolution, cached or not: the topo pre-pass
    // in Compile warms the cache for every live node, and a fan-out port resolves
    // against a different target width per consumer.
    std::string expr = it->second;
    const std::string targetGraph = GlslTypeToGraphType(targetGlslType);

    if (targetGlslType == "vec3" && expr.rfind("vec4(", 0) == 0)
        expr += ".rgb";
    else if (targetGraph == "float3" && expr.find("vec4") == 0)
        expr += ".rgb";
    if (targetGlslType == "float" && (expr.find("vec2") == 0 || expr.find("vec3") == 0 || expr.find("vec4") == 0))
    {
        ctx.result.Errors.push_back(
            {"ShaderGraph: vector->float requires explicit node at " + nodeId + "." + portId, nodeId});
    }
    if (targetGlslType != "float" && expr.find("vec") != 0 && targetGlslType.find("vec") == 0)
        expr = BroadcastScalarToVector(expr, targetGlslType);

    return expr;
}

std::string DefaultTextureNameForNode(std::string_view typeId)
{
    if (typeId == "SampleTextureArray")
        return "textureArrayMap";
    if (typeId == "SampleCubemap")
        return "cubemap";
    if (typeId == "SampleTexture2D" || typeId == "SampleTexture")
        return "albedoMap";
    if (typeId == "TriplanarTexture")
        return "albedoMap";
    return {};
}

std::optional<std::string> FindNodePortDefault(const SgGraphNode& node, const SgPortMeta& port)
{
    auto defIt = node.PortDefaults.find(port.Name);
    if (defIt != node.PortDefaults.end() && !defIt->second.empty())
        return defIt->second;

    if (port.Name == "tex" || port.GraphType == "texture2d" || port.GraphType == "texture2d_array" ||
        port.GraphType == "texture_cube")
    {
        auto textureIt = node.PortDefaults.find("texture");
        if (textureIt != node.PortDefaults.end() && !textureIt->second.empty())
            return textureIt->second;
    }
    return std::nullopt;
}

std::string DefaultSurfacePortExpression(const SgPortMeta& port)
{
    if (port.GlslType == "vec3")
    {
        if (port.Name == "normal")
            return "normalize(sIn.normalWS)";
        if (port.Name == "view")
            return "normalize(sIn.viewDirWS)";
        if (port.Name == "position" || port.Name == "worldPosition")
            return "sIn.positionWS";
    }
    if (port.GlslType == "vec2" && (port.Name == "uv" || port.Name == "UV0"))
        return "sIn.uv0";
    return {};
}

std::string GetInputForPort(CompileContext& ctx, const SgGraphNode& node, const SgPortMeta& port, bool vertexStage)
{
    if (const SgGraphEdge* edge = FindEdgeTo(ctx.doc, node.Id, port.Name))
        return ResolveSourceExpr(ctx, *edge, port.GlslType, vertexStage);

    if (const std::optional<std::string> portDefault = FindNodePortDefault(node, port))
        return *portDefault;

    if (port.Name == "tex")
    {
        if (const std::string defaultTexture = DefaultTextureNameForNode(node.TypeId); !defaultTexture.empty())
            return defaultTexture;
    }

    if (!vertexStage)
    {
        if (const std::string surfaceDefault = DefaultSurfacePortExpression(port); !surfaceDefault.empty())
            return surfaceDefault;
    }

    if (!port.DefaultExpression.empty())
        return port.DefaultExpression;

    return ZeroExpressionForType(port.GlslType);
}

bool NormalInputUsesTangentSpace(const SgGraphDocument& doc, const SgGraphEdge& edge)
{
    if (IsPropertyName(doc, edge.SourceNodeId) || IsTextureName(doc, edge.SourceNodeId))
        return true;

    const SgGraphNode* src = FindNode(doc, edge.SourceNodeId);
    if (!src)
        return false;

    if (src->TypeId == "NormalVector")
        return false;

    const std::string typeId = ResolveNodeTypeAlias(src->TypeId);
    if (typeId == "EngineInput")
        return edge.SourcePortId != "WorldNormal";

    static const std::unordered_set<std::string> kTangentSpaceNormalNodes = {
        "SampleNormal",
        "NormalBlend",
        "NormalStrength",
        "NormalUnpack",
        "NormalFromHeight",
        "BlendNormals",
    };
    if (kTangentSpaceNormalNodes.count(typeId) > 0)
        return true;

    return false;
}

void CompileTriplanarTextureNode(CompileContext& ctx, const SgGraphNode& node, bool vertexStage)
{
    (void)vertexStage;
    const std::string prefix = SanitizeTemp(node.Id);

    SgPortMeta texPort;
    texPort.Name = "tex";
    texPort.GlslType = "sampler2D";
    texPort.GraphType = "texture2d";
    const std::string tex = GetInputForPort(ctx, node, texPort, false);

    auto editorInput = [&](const char* portName, const char* glslType, const char* fallback) -> std::string {
        SgPortMeta port;
        port.Name = portName;
        port.GlslType = glslType;
        port.GraphType = GlslTypeToGraphType(glslType);
        const std::string expr = GetInputForPort(ctx, node, port, false);
        return expr == "0.0" ? fallback : expr;
    };

    const std::string position = editorInput("position", "vec3", "sIn.positionWS");
    const std::string normal = editorInput("normal", "vec3", "normalize(sIn.normalWS)");
    const std::string sharpness = editorInput("sharpness", "float", "4.0");
    const std::string tiling = editorInput("scale", "float", "1.0");

    const std::string weightsVar = prefix + "_weights";
    const std::string rgbVar = prefix + "_rgb";
    std::ostringstream decl;
    decl << "vec3 " << weightsVar << " = SG_TriplanarWeights(" << normal << ", " << sharpness << ");\n";
    decl << "vec3 " << rgbVar << ";\n";
    decl << "SG_TriplanarSample(" << tex << ", " << position << ", " << weightsVar << ", " << tiling << ", "
         << rgbVar << ");\n";
    if (ctx.nodeBody)
        *ctx.nodeBody += decl.str();

    ctx.portExprs[PortKey(node.Id, "rgb")] = rgbVar;
    ctx.portExprs[PortKey(node.Id, "color")] = "vec4(" + rgbVar + ", 1.0)";
}

void CompileNodeInstance(CompileContext& ctx, const SgGraphNode& node, bool vertexStage)
{
    if (!ctx.visited.insert(node.Id).second)
    {
        ctx.result.Errors.push_back({"ShaderGraph: cyclic dependency at node '" + node.Id + "'.", node.Id});
        return;
    }

    if (node.TypeId == "TriplanarTexture")
    {
        CompileTriplanarTextureNode(ctx, node, vertexStage);
        return;
    }

    const std::string typeId = ResolveNodeTypeAlias(node.TypeId);

    if (typeId == "FloatParameter" || typeId == "Vec2Parameter" || typeId == "Vec3Parameter" ||
        typeId == "Vec4Parameter" || typeId == "ColorParameter")
    {
        ctx.portExprs[PortKey(node.Id, "value")] = ParameterSlotExpression(node, typeId);
        return;
    }

    if (typeId == "Billboard" && vertexStage)
    {
        SgPortMeta positionPort;
        positionPort.Name = "position";
        positionPort.GlslType = "vec3";
        positionPort.DefaultExpression = "position";
        const std::string pos = GetInputForPort(ctx, node, positionPort, vertexStage);
        ctx.portExprs[PortKey(node.Id, "out")] = "SG_Billboard(" + pos + ", inst)";
        return;
    }

    const SgNodeDefinition* def = FindDefinition(ctx.library, typeId);
    if (!def)
    {
        ctx.result.Errors.push_back({"ShaderGraph: unknown node type '" + node.TypeId + "'.", node.Id});
        return;
    }

    if (typeId == "EngineInput")
    {
        for (const auto& port : def->Overloads[0].Outputs)
        {
            if (const auto expr = GetEngineInputExpr(port.Name, vertexStage))
            {
                ctx.portExprs[PortKey(node.Id, port.Name)] = *expr;
                continue;
            }
            /* Only report a port the vertex path actually reads: EngineInput
               declares every output, and most graphs wire one or two. */
            if (!FindEdgeFrom(ctx.doc, node.Id, port.Name))
                continue;
            ctx.result.Errors.push_back({"ShaderGraph: engine input '" + port.Name +
                                             "' is not available in the vertex stage (node '" +
                                             node.Id + "' feeds Vertex Pos).",
                                         node.Id});
            ctx.portExprs[PortKey(node.Id, port.Name)] = ZeroExpressionForType(port.GlslType);
        }
        return;
    }

    if (typeId == "SurfaceOutput" || typeId == "VertexOutput" || typeId == "Group" || typeId == "Comment")
        return;

    if (def->Overloads.empty())
        return;

    std::vector<std::string> argTypes;
    std::vector<std::string> argExprs;
    const SgNodeOverload* overload = nullptr;

    for (const auto& candidate : def->Overloads)
    {
        std::vector<std::string> trialTypes;
        std::vector<std::string> trialExprs;
        trialTypes.reserve(candidate.Inputs.size());
        trialExprs.reserve(candidate.Inputs.size());
        for (const auto& input : candidate.Inputs)
        {
            trialTypes.push_back(input.GraphType);
            trialExprs.push_back(GetInputForPort(ctx, node, input, vertexStage));
        }
        if (PickOverload(*def, trialTypes) == &candidate)
        {
            overload = &candidate;
            argTypes = std::move(trialTypes);
            argExprs = std::move(trialExprs);
            break;
        }
    }

    if (!overload)
    {
        ctx.result.Errors.push_back({"ShaderGraph: no matching overload for node '" + node.TypeId + "'.", node.Id});
        return;
    }

    const std::string prefix = SanitizeTemp(node.Id);
    std::ostringstream decl;

    if (overload->Outputs.size() == 1 && overload->Outputs[0].Name == "result" &&
        overload->Outputs[0].GlslType != "void")
    {
        const std::string temp = prefix + "_result";
        decl << overload->Outputs[0].GlslType << " " << temp << " = " << overload->FunctionName << "(";
        for (size_t i = 0; i < argExprs.size(); ++i)
        {
            if (i > 0)
                decl << ", ";
            decl << argExprs[i];
        }
        decl << ");\n";
        if (ctx.nodeBody)
            *ctx.nodeBody += decl.str();
        ctx.portExprs[PortKey(node.Id, "result")] = temp;
        ctx.portExprs[PortKey(node.Id, overload->Outputs[0].Name)] = temp;
        ctx.portExprs[PortKey(node.Id, "value")] = temp;
        ctx.portExprs[PortKey(node.Id, "out")] = temp;
        if (overload->Outputs[0].GlslType == "vec4")
            ctx.portExprs[PortKey(node.Id, "color")] = temp;
        return;
    }

    for (const auto& output : overload->Outputs)
    {
        if (output.GlslType == "void")
            continue;
        decl << output.GlslType << " " << prefix << "_" << output.Name << ";\n";
    }
    decl << overload->FunctionName << "(";
    for (size_t i = 0; i < argExprs.size(); ++i)
    {
        if (i > 0)
            decl << ", ";
        decl << argExprs[i];
    }
    for (const auto& output : overload->Outputs)
    {
        if (!output.Name.empty() && output.Name != "result")
        {
            decl << ", " << prefix << "_" << output.Name;
        }
    }
    decl << ");\n";
    if (ctx.nodeBody)
        *ctx.nodeBody += decl.str();

    for (const auto& output : overload->Outputs)
    {
        if (output.Name.empty())
            continue;
        ctx.portExprs[PortKey(node.Id, output.Name)] = prefix + "_" + output.Name;
    }
}

void CollectUpstream(const SgGraphDocument& doc, const std::string& nodeId, std::unordered_set<std::string>& live)
{
    if (!live.insert(nodeId).second)
        return;
    for (const auto& edge : doc.Edges)
    {
        if (edge.TargetNodeId != nodeId)
            continue;
        if (IsPropertyName(doc, edge.SourceNodeId) || IsTextureName(doc, edge.SourceNodeId))
            continue;
        CollectUpstream(doc, edge.SourceNodeId, live);
    }
}

std::vector<std::string> TopoSort(const SgGraphDocument& doc, const std::unordered_set<std::string>& live)
{
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (const auto& id : live)
        indegree[id] = 0;

    for (const auto& edge : doc.Edges)
    {
        if (!live.count(edge.TargetNodeId) || !live.count(edge.SourceNodeId))
            continue;
        if (IsPropertyName(doc, edge.SourceNodeId) || IsTextureName(doc, edge.SourceNodeId))
            continue;
        adj[edge.SourceNodeId].push_back(edge.TargetNodeId);
        ++indegree[edge.TargetNodeId];
    }

    std::vector<std::string> queue;
    for (const auto& [id, deg] : indegree)
    {
        if (deg == 0)
            queue.push_back(id);
    }

    std::vector<std::string> order;
    while (!queue.empty())
    {
        const std::string n = queue.back();
        queue.pop_back();
        order.push_back(n);
        for (const auto& next : adj[n])
        {
            if (--indegree[next] == 0)
                queue.push_back(next);
        }
    }
    return order;
}

} // namespace

SgCompileResult SgGraphCompiler::Compile(const SgGraphDocument& doc, const SgNodeLibraryIndex& library)
{
    SgCompileResult result;
    result.Properties = doc.Properties;
    result.Textures = doc.Textures;
    result.VariantDefines = doc.VariantDefines;

    const SgGraphNode* surfaceSink = nullptr;
    const SgGraphNode* vertexSink = nullptr;
    for (const auto& node : doc.Nodes)
    {
        const std::string t = ResolveNodeTypeAlias(node.TypeId);
        if (t == "SurfaceOutput")
            surfaceSink = &node;
        if (t == "VertexOutput")
            vertexSink = &node;
    }

    if (!surfaceSink)
    {
        result.Errors.push_back({"ShaderGraph: missing SurfaceOutput node.", std::string{}});
        result.Success = false;
        return result;
    }

    std::unordered_set<std::string> surfaceLive;
    CollectUpstream(doc, surfaceSink->Id, surfaceLive);

    std::unordered_set<std::string> vertexLive;
    if (vertexSink)
        CollectUpstream(doc, vertexSink->Id, vertexLive);

    std::string surfaceNodeBody;
    CompileContext surfaceCtx{doc, library, result, &surfaceNodeBody, {}, {}, {}, false};
    const auto surfaceOrder = TopoSort(doc, surfaceLive);
    for (const auto& nodeId : surfaceOrder)
    {
        if (const SgGraphNode* n = FindNode(doc, nodeId))
            CompileNodeInstance(surfaceCtx, *n, false);
    }

    std::ostringstream surfaceFn;
    surfaceFn << "#ifndef GE_STAGE_VERTEX\nSurfaceOutput EvaluateSurface(SurfaceInput sIn)\n{\n";
    surfaceFn << "    SurfaceOutput o = DefaultSurfaceOutput();\n";
    surfaceFn << surfaceNodeBody;

    auto emitSinkInput = [&](const char* port, const char* field, const char* glslType, const char* defExpr)
    {
        if (const SgGraphEdge* edge = FindEdgeTo(doc, surfaceSink->Id, port))
        {
            std::string expr = ResolveSourceExpr(surfaceCtx, *edge, glslType, false);
            if (std::string(port) == "Normal")
            {
                if (NormalInputUsesTangentSpace(doc, *edge))
                {
                    surfaceCtx.wiringNormalTs = true;
                    surfaceCtx.variantDefines.push_back("HAS_TANGENT");
                    expr = "normalize(sIn.TBN * " + expr + ")";
                }
                else
                {
                    expr = "normalize(" + expr + ")";
                }
            }
            surfaceFn << "    o." << field << " = " << expr << ";\n";
        }
        else
            (void)defExpr;
    };

    emitSinkInput("BaseColor", "baseColor", "vec3", "vec3(1.0)");
    emitSinkInput("Metallic", "metallic", "float", "0.0");
    emitSinkInput("Roughness", "roughness", "float", "0.5");
    emitSinkInput("Normal", "normalWS", "vec3", "sIn.normalWS");
    emitSinkInput("Emissive", "emissive", "vec3", "vec3(0.0)");
    emitSinkInput("Opacity", "opacity", "float", "1.0");
    emitSinkInput("AmbientOcclusion", "ao", "float", "1.0");

    surfaceFn << "    return o;\n}\n#endif // !GE_STAGE_VERTEX\n";
    result.Body = surfaceFn.str();

    if (vertexSink)
    {
        std::string vertexNodeBody;
        CompileContext vertexCtx{doc, library, result, &vertexNodeBody, {}, {}, {}, false};
        vertexCtx.visited.clear();
        const auto vertexOrder = TopoSort(doc, vertexLive);
        for (const auto& nodeId : vertexOrder)
        {
            if (const SgGraphNode* n = FindNode(doc, nodeId))
                CompileNodeInstance(vertexCtx, *n, true);
        }

        /* One generated file serves both stages: the composer binds it as the
           vertex modifier too, and the vertex adapter has no SurfaceInput. */
        std::ostringstream vertexFn;
        vertexFn << "\n#ifdef GE_STAGE_VERTEX\nvec3 ModifyVertex(vec3 position, InstanceData inst)\n{\n";
        vertexFn << vertexNodeBody;
        if (const SgGraphEdge* edge = FindEdgeTo(doc, vertexSink->Id, "PositionOffset"))
        {
            std::string expr = ResolveSourceExpr(vertexCtx, *edge, "vec3", true);
            vertexFn << "    return position + " << expr << ";\n";
        }
        else
            vertexFn << "    return position;\n";
        vertexFn << "}\n#endif // GE_STAGE_VERTEX\n";
        result.Body += vertexFn.str();
        result.VariantDefines.push_back("HAS_VERTEX_MODIFIER");
    }

    result.VariantDefines.insert(result.VariantDefines.end(), surfaceCtx.variantDefines.begin(),
                                 surfaceCtx.variantDefines.end());
    result.Success = result.Errors.empty();
    return result;
}

} // namespace GameEngine::ShaderGraph

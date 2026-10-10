#include "Graph/SgGraphModelBridge.h"

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgPropertyBinding.h"
#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Rendering/ShaderGraph/SgTypes.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{
namespace ShaderGraph
{
std::string ResolveNodeTypeAlias(const std::string& typeId);
}
namespace
{

using namespace ShaderGraph;

std::unordered_map<std::string, std::string> ToStringMap(const Graph::GraphObject& object)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [key, value] : object)
        out.emplace(key, value.ToString());
    return out;
}

Graph::GraphObject FromStringMap(const std::unordered_map<std::string, std::string>& map)
{
    Graph::GraphObject object;
    for (const auto& [key, value] : map)
        object[key] = value;
    return object;
}

struct EditorInputPortMapping
{
    const char* TypeId;
    const char* OutPortId;
};

const EditorInputPortMapping* MapSgEnginePortToEditor(const std::string& sgPort)
{
    static const std::pair<const char*, EditorInputPortMapping> kTable[] = {
        {"UV0", {"UV", "out"}},
        {"UV1", {"UV1", "out"}},
        {"WorldNormal", {"NormalVector", "normal"}},
        {"ViewDirection", {"ViewDirection", "view"}},
        {"WorldPosition", {"WorldPosition", "out"}},
        {"WorldTangent", {"Tangent", "out"}},
        {"Time", {"Time", "out"}},
        {"ScreenUV", {"ScreenUV", "out"}},
        {"VertexColor", {"VertexColor", "out"}},
    };

    for (const auto& [port, mapping] : kTable)
    {
        if (sgPort == port)
            return &mapping;
    }
    return nullptr;
}

void ExpandEngineInputNodes(Graph::Model& model)
{
    std::vector<std::string> engineInputIds;
    engineInputIds.reserve(model.Nodes.size());
    for (const Graph::Node& node : model.Nodes)
    {
        if (node.TypeId == "EngineInput")
            engineInputIds.push_back(node.Id);
    }
    if (engineInputIds.empty())
        return;

    for (const std::string& einId : engineInputIds)
    {
        const Graph::Node* einNode = model.FindNode(einId);
        if (!einNode)
            continue;

        const float baseX = einNode->PositionX;
        const float baseY = einNode->PositionY;
        std::unordered_map<std::string, std::string> portToNodeId;
        int offset = 0;

        for (Graph::Edge& link : model.Links)
        {
            if (link.SourceNodeId != einId)
                continue;

            const EditorInputPortMapping* mapping = MapSgEnginePortToEditor(link.SourcePortId);
            if (!mapping)
                continue;

            std::string& replacementId = portToNodeId[link.SourcePortId];
            if (replacementId.empty())
            {
                Graph::Node node;
                node.Id = model.GenerateNodeId();
                node.TypeId = mapping->TypeId;
                node.PositionX = baseX;
                node.PositionY = baseY + static_cast<float>(offset) * 80.f;
                ++offset;
                model.Nodes.push_back(std::move(node));
                replacementId = model.Nodes.back().Id;
            }

            link.SourceNodeId = replacementId;
            link.SourcePortId = mapping->OutPortId;
        }

        model.Nodes.erase(std::remove_if(model.Nodes.begin(), model.Nodes.end(),
                                         [&](const Graph::Node& node) { return node.Id == einId; }),
                          model.Nodes.end());
    }
}

std::string MapPortIdToSg(const std::string& typeId, const std::string& portId)
{
    if (typeId == "UV")
        return "UV0";
    if (typeId == "UV1")
        return "UV1";
    if (typeId == "NormalVector")
        return "WorldNormal";
    if (typeId == "ViewDirection")
        return "ViewDirection";
    if (typeId == "WorldPosition")
        return "WorldPosition";
    if (typeId == "Time")
        return "Time";
    if (typeId == "ScreenUV")
        return "ScreenUV";
    if (typeId == "VertexColor")
        return "VertexColor";
    if (typeId == "Tangent")
        return "WorldTangent";
    return portId;
}

std::string MapSourcePortIdToSg(const std::string& typeId, const std::string& portId)
{
    if (typeId == "TriplanarTexture" && portId == "color")
        return "rgb";
    return MapPortIdToSg(typeId, portId);
}

void EnsureDocumentTexture(SgGraphDocument& doc, const std::string& name)
{
    if (name.empty())
        return;
    for (const SgGraphTexture& existing : doc.Textures)
    {
        if (existing.Name == name)
            return;
    }
    SgGraphTexture texture;
    texture.Name = name;
    doc.Textures.push_back(std::move(texture));
}

std::string DefaultVariableValueForGraphType(std::string_view graphType)
{
    if (graphType == "bool")
        return "false";
    if (graphType == "string")
        return "";
    if (graphType == "float2")
        return "0, 0";
    if (graphType == "float3")
        return "0, 0, 0";
    if (graphType == "float4")
        return "0, 0, 0, 0";
    return "0";
}

Graph::Variable SgPropertyToGraphVariable(const SgGraphProperty& prop, std::uint64_t createdOrder)
{
    Graph::Variable variable;
    variable.Name = prop.Name;
    variable.Type = GlslTypeToGraphType(prop.Type);
    variable.Value = prop.DefaultValue.empty() ? DefaultVariableValueForGraphType(variable.Type) : prop.DefaultValue;
    variable.IsPublic = prop.IsPublic;
    variable.CreatedOrder = createdOrder;
    variable.RangeMin = prop.RangeMin;
    variable.RangeMax = prop.RangeMax;
    variable.Hint = prop.Hint;
    return variable;
}

} // namespace

bool IsMaterialParameterNodeType(const std::string& typeId)
{
    return typeId == "FloatParameter" || typeId == "Vec2Parameter" || typeId == "Vec3Parameter" ||
           typeId == "Vec4Parameter" || typeId == "ColorParameter";
}

const Graph::Variable* FindGraphVariableByName(const Graph::Model& model, const std::string& name)
{
    for (const Graph::Variable& variable : model.Variables)
    {
        if (variable.Name == name)
            return &variable;
    }
    return nullptr;
}

ShaderGraph::SgGraphProperty GraphVariableToSgProperty(const Graph::Variable& variable)
{
    SgGraphProperty prop;
    prop.Name = variable.Name;
    prop.Type = ShaderGraph::GraphTypeToGlslType(variable.Type);
    prop.DefaultValue = variable.Value;
    prop.IsPublic = variable.IsPublic;
    prop.RangeMin = variable.RangeMin;
    prop.RangeMax = variable.RangeMax;
    prop.Hint = variable.Hint;
    return prop;
}

void AddShaderGraphPublicPropertyMetadata(const SgGraphProperty& prop,
                                          const SgPublicPropertyBinding& binding,
                                          MaterialDocument& doc)
{
    if (!prop.IsPublic ||
        !ShaderGraph::UsesDedicatedShaderGraphInspectorRow(binding, prop.Name))
    {
        return;
    }

    for (const MaterialShaderGraphPublicProperty& existing : doc.shaderGraphPublicProperties)
    {
        if (existing.GraphName == prop.Name)
            return;
    }

    MaterialShaderGraphPublicProperty entry;
    entry.GraphName = prop.Name;
    entry.Type = prop.Type;
    entry.MaterialPropertyKeys = binding.MaterialPropertyKeys;
    doc.shaderGraphPublicProperties.push_back(std::move(entry));
}

void SyncMaterialParameterNodeSlots(Graph::Model& model)
{
    if (model.KindId != Graph::kKindIdMaterial)
        return;

    for (Graph::Node& node : model.Nodes)
    {
        if (!IsMaterialParameterNodeType(node.TypeId))
            continue;

        auto nameIt = node.Parameters.find("variableName");
        if (nameIt == node.Parameters.end())
            continue;

        const Graph::Variable* variable = FindGraphVariableByName(model, nameIt->second.ToString());
        if (!variable)
            continue;

        const std::string glslType = ShaderGraph::GraphTypeToGlslType(variable->Type);
        const auto binding = ShaderGraph::ResolvePublicPropertyBinding(variable->Name, glslType);
        if (!binding)
            continue;

        node.Parameters["slot"] = std::to_string(binding->ParameterSlot);
        if (node.TypeId == "FloatParameter" && !binding->FloatComponent.empty())
            node.Parameters["component"] = binding->FloatComponent;
        else if (!binding->VectorSwizzle.empty())
            node.Parameters["swizzle"] = binding->VectorSwizzle;
    }
}

void ApplyMaterialGraphVariablesToPreviewDocument(const Graph::Model& model, MaterialDocument& doc)
{
    doc.shaderGraphPublicProperties.clear();
    std::unordered_set<std::string> appliedVariables;

    for (const Graph::Node& node : model.Nodes)
    {
        if (!IsMaterialParameterNodeType(node.TypeId))
            continue;

        auto nameIt = node.Parameters.find("variableName");
        if (nameIt == node.Parameters.end() || nameIt->second.empty())
            continue;

        const Graph::Variable* variable = FindGraphVariableByName(model, nameIt->second.ToString());
        if (!variable)
            continue;

        const ShaderGraph::SgGraphProperty prop = GraphVariableToSgProperty(*variable);

        int slot = 0;
        bool hasExplicitSlot = false;
        if (auto slotIt = node.Parameters.find("slot"); slotIt != node.Parameters.end() && !slotIt->second.empty())
        {
            try
            {
                slot = std::stoi(slotIt->second.ToString());
                hasExplicitSlot = true;
            }
            catch (...)
            {
                slot = 0;
            }
        }
        if (!hasExplicitSlot)
        {
            if (const auto nameBinding = ShaderGraph::ResolvePublicPropertyBinding(prop.Name, prop.Type))
                slot = nameBinding->ParameterSlot;
        }

        std::string componentStorage = "x";
        if (auto compIt = node.Parameters.find("component"); compIt != node.Parameters.end() && !compIt->second.empty())
            componentStorage = compIt->second.ToString();
        else if (!hasExplicitSlot)
        {
            if (const auto nameBinding = ShaderGraph::ResolvePublicPropertyBinding(prop.Name, prop.Type);
                nameBinding && !nameBinding->FloatComponent.empty())
            {
                componentStorage = std::string(nameBinding->FloatComponent);
            }
        }
        std::string_view component = componentStorage;

        const auto binding =
            ShaderGraph::ResolveParameterSlotMaterialBinding(slot, node.TypeId, component);
        if (!binding)
            continue;

        ShaderGraph::ApplyParameterNodeValueToMaterialDocument(prop, *binding, doc);
        AddShaderGraphPublicPropertyMetadata(prop, *binding, doc);
        appliedVariables.insert(variable->Name);
    }

    for (const Graph::Variable& variable : model.Variables)
    {
        if (variable.Name.empty() || appliedVariables.count(variable.Name))
            continue;

        const ShaderGraph::SgGraphProperty prop = GraphVariableToSgProperty(variable);
        const auto binding = ShaderGraph::ResolvePublicPropertyBinding(prop.Name, prop.Type);
        if (binding)
        {
            ShaderGraph::ApplyPublicPropertyToMaterialDocument(prop, *binding, doc);
            AddShaderGraphPublicPropertyMetadata(prop, *binding, doc);
        }
    }
}

ShaderGraph::SgGraphDocument GraphModelToSgDocument(const Graph::Model& model, const std::string& graphName)
{
    SgGraphDocument doc;
    doc.GraphName = graphName.empty() ? "MaterialGraph" : graphName;
    doc.ViewportPanX = model.Viewport.PanX;
    doc.ViewportPanY = model.Viewport.PanY;
    doc.ViewportZoom = model.Viewport.Zoom;
    if (!model.LightingModel.empty())
        doc.LightingModel = model.LightingModel;
    doc.VariantDefines = model.VariantDefines;
    doc.UnknownTags = model.UnknownSgTags;

    doc.Textures.reserve(model.Textures.size());
    for (const Graph::Texture& texture : model.Textures)
    {
        if (texture.Name.empty())
            continue;
        SgGraphTexture sgTex;
        sgTex.Name = texture.Name;
        sgTex.Guid = texture.Guid;
        sgTex.Hint = texture.Hint;
        doc.Textures.push_back(std::move(sgTex));
    }

    doc.Properties.reserve(model.Variables.size());
    for (const auto& variable : model.Variables)
    {
        if (variable.Name.empty())
            continue;
        doc.Properties.push_back(GraphVariableToSgProperty(variable));
    }

    const bool hasVertex =
        std::any_of(model.Nodes.begin(), model.Nodes.end(), [](const Graph::Node& node) {
            return ResolveNodeTypeAlias(node.TypeId) == "VertexOutput";
        });
    doc.Stage = hasVertex ? SgGraphStage::Both : SgGraphStage::Surface;

    std::string engineInputId;
    for (const auto& node : model.Nodes)
    {
        if (node.TypeId == "Group" || node.TypeId == "Comment")
            continue;

        const bool isInput = node.TypeId == "UV" || node.TypeId == "UV1" || node.TypeId == "Time" ||
                             node.TypeId == "NormalVector" || node.TypeId == "ViewDirection" ||
                             node.TypeId == "WorldPosition" || node.TypeId == "ScreenUV" ||
                             node.TypeId == "VertexColor" || node.TypeId == "Tangent";

        if (isInput)
        {
            if (engineInputId.empty())
            {
                engineInputId = "ein";
                SgGraphNode ein;
                ein.Id = engineInputId;
                ein.TypeId = "EngineInput";
                ein.PositionX = node.PositionX;
                ein.PositionY = node.PositionY;
                doc.Nodes.push_back(std::move(ein));
            }
            continue;
        }

        SgGraphNode sgNode;
        sgNode.Id = node.Id;
        if (node.TypeId == "TriplanarTexture")
            sgNode.TypeId = "TriplanarTexture";
        else
            sgNode.TypeId = ResolveNodeTypeAlias(node.TypeId);
        sgNode.PositionX = node.PositionX;
        sgNode.PositionY = node.PositionY;
        sgNode.PortDefaults = ToStringMap(node.Parameters);
        doc.Nodes.push_back(std::move(sgNode));
    }

    for (const Graph::Node& node : model.Nodes)
    {
        auto textureIt = node.Parameters.find("texture");
        if (textureIt != node.Parameters.end())
            EnsureDocumentTexture(doc, textureIt->second.ToString());
    }

    for (const auto& link : model.Links)
    {
        SgGraphEdge edge;
        const Graph::Node* src = model.FindNode(link.SourceNodeId);
        const Graph::Node* tgt = model.FindNode(link.TargetNodeId);

        if (src && (src->TypeId == "UV" || src->TypeId == "UV1" || src->TypeId == "Time" ||
                    src->TypeId == "NormalVector" || src->TypeId == "ViewDirection" ||
                    src->TypeId == "WorldPosition" || src->TypeId == "ScreenUV" ||
                    src->TypeId == "VertexColor" || src->TypeId == "Tangent"))
        {
            edge.SourceNodeId = engineInputId.empty() ? link.SourceNodeId : engineInputId;
            edge.SourcePortId = MapPortIdToSg(src->TypeId, link.SourcePortId);
        }
        else
        {
            edge.SourceNodeId = link.SourceNodeId;
            edge.SourcePortId =
                src ? MapSourcePortIdToSg(src->TypeId, link.SourcePortId) : link.SourcePortId;
        }

        edge.TargetNodeId = link.TargetNodeId;
        edge.TargetPortId = tgt ? MapPortIdToSg(tgt->TypeId, link.TargetPortId) : link.TargetPortId;

        doc.Edges.push_back(std::move(edge));
    }

    return doc;
}

Graph::Model SgDocumentToGraphModel(const ShaderGraph::SgGraphDocument& doc)
{
    Graph::Model model;
    model.KindId.assign(Graph::kKindIdMaterial);
    model.Viewport.PanX = doc.ViewportPanX;
    model.Viewport.PanY = doc.ViewportPanY;
    model.Viewport.Zoom = doc.ViewportZoom;
    model.LightingModel = doc.LightingModel;
    model.VariantDefines = doc.VariantDefines;
    model.UnknownSgTags = doc.UnknownTags;

    model.Textures.clear();
    model.Textures.reserve(doc.Textures.size());
    for (const SgGraphTexture& tex : doc.Textures)
    {
        if (tex.Name.empty())
            continue;
        Graph::Texture graphTex;
        graphTex.Name = tex.Name;
        graphTex.Guid = tex.Guid;
        graphTex.Hint = tex.Hint;
        model.Textures.push_back(std::move(graphTex));
    }

    model.Variables.reserve(doc.Properties.size());
    std::uint64_t createdOrder = 0;
    for (const auto& prop : doc.Properties)
    {
        if (prop.Name.empty())
            continue;
        model.Variables.push_back(SgPropertyToGraphVariable(prop, createdOrder++));
    }

    for (const auto& sgNode : doc.Nodes)
    {
        Graph::Node node;
        node.Id = sgNode.Id;
        node.TypeId = ResolveNodeTypeAlias(sgNode.TypeId);
        node.PositionX = sgNode.PositionX;
        node.PositionY = sgNode.PositionY;
        node.Parameters = FromStringMap(sgNode.PortDefaults);
        model.Nodes.push_back(std::move(node));
    }

    for (const auto& edge : doc.Edges)
    {
        Graph::Edge link;
        link.Id = edge.SourceNodeId + "_" + edge.TargetNodeId + "_" + edge.TargetPortId;
        link.SourceNodeId = edge.SourceNodeId;
        link.SourcePortId = edge.SourcePortId;
        link.TargetNodeId = edge.TargetNodeId;
        link.TargetPortId = edge.TargetPortId;
        model.Links.push_back(std::move(link));
    }

    ExpandEngineInputNodes(model);
    SyncMaterialParameterNodeSlots(model);
    return model;
}

namespace
{

/** Length-prefixed so field boundaries are part of the hash: without it
    {Id="ab", Type="c"} and {Id="a", Type="bc"} digest the same. */
std::uint64_t HashText(std::uint64_t seed, const std::string& text)
{
    return Hashing::Fnv1a64(text, Hashing::Fnv1a64Value(seed, text.size()));
}

} // namespace

std::uint64_t MaterialGraphSurfaceDigest(const Graph::Model& model)
{
    // The materialized projection, not the raw model: the bridge is what decides
    // which node types collapse into pseudo-nodes and how pins are renamed, so
    // hashing its output is what makes the digest agree with the compiler.
    const ShaderGraph::SgGraphDocument doc = GraphModelToSgDocument(model, "digest");

    std::uint64_t hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, doc.Stage);
    hash = HashText(hash, doc.LightingModel);
    for (const std::string& define : doc.VariantDefines)
        hash = HashText(hash, define);
    for (const std::string& tag : doc.UnknownTags)
        hash = HashText(hash, tag);
    for (const ShaderGraph::SgGraphTexture& texture : doc.Textures)
    {
        hash = HashText(hash, texture.Name);
        hash = HashText(hash, texture.Guid);
        hash = HashText(hash, texture.Hint);
    }
    // Declarations only: a property compiles to a uniform reference by name and
    // type, and its value is pushed to the runtime without touching the shader.
    for (const ShaderGraph::SgGraphProperty& property : doc.Properties)
    {
        hash = HashText(hash, property.Name);
        hash = HashText(hash, property.Type);
    }
    for (const ShaderGraph::SgGraphNode& node : doc.Nodes)
    {
        hash = HashText(hash, node.Id);
        hash = HashText(hash, node.TypeId);
        // PortDefaults is unordered, so fold each entry commutatively rather
        // than letting bucket order decide the digest.
        std::uint64_t ports = 0;
        for (const auto& [port, value] : node.PortDefaults)
            ports += HashText(HashText(Hashing::kFnv1a64OffsetBasis, port), value);
        hash = Hashing::Fnv1a64Value(hash, ports);
    }
    for (const ShaderGraph::SgGraphEdge& edge : doc.Edges)
    {
        hash = HashText(hash, edge.SourceNodeId);
        hash = HashText(hash, edge.SourcePortId);
        hash = HashText(hash, edge.TargetNodeId);
        hash = HashText(hash, edge.TargetPortId);
    }
    return hash;
}

} // namespace GameEngine

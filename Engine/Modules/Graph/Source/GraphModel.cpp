#include "Graph/GraphModel.h"
#include "Graph/GraphTypeRegistry.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace GameEngine {
namespace Graph {

static std::string PortDirectionToString(PortDirection d)
{
    return d == PortDirection::In ? "in" : "out";
}

static PortDirection PortDirectionFromString(const std::string& s)
{
    return (s == "out") ? PortDirection::Out : PortDirection::In;
}

static std::string DefaultVariableValueForType(const std::string& type)
{
    if (type == "bool")
        return "false";
    if (type == "string")
        return "";
    if (type == "float2")
        return "0, 0";
    if (type == "float3")
        return "0, 0, 0";
    if (type == "float4")
        return "0, 0, 0, 0";
    if (type == "color")
        return "1, 1, 1";
    return "0";
}

bool Node::AssignParameterFromText(const std::string& key, std::string_view text)
{
    auto it = Parameters.find(key);
    if (it != Parameters.end())
    {
        if (it->second.EqualsString(text))
            return true;
        return it->second.AssignFromText(text);
    }
    Parameters.emplace(key, GraphValue(std::string(text)));
    return true;
}

std::string Model::GenerateNodeId() const
{
    size_t n = 0;
    std::unordered_set<std::string> existing;
    for (const auto& node : Nodes)
        existing.insert(node.Id);
    while (true)
    {
        std::string id = "node_" + std::to_string(n);
        if (existing.find(id) == existing.end())
            return id;
        ++n;
    }
}

std::string Model::GenerateLinkId() const
{
    size_t n = 0;
    std::unordered_set<std::string> existing;
    for (const auto& link : Links)
        existing.insert(link.Id);
    while (true)
    {
        std::string id = "link_" + std::to_string(n);
        if (existing.find(id) == existing.end())
            return id;
        ++n;
    }
}

Node* Model::FindNode(const std::string& nodeId)
{
    for (auto& n : Nodes)
        if (n.Id == nodeId)
            return &n;
    return nullptr;
}

const Node* Model::FindNode(const std::string& nodeId) const
{
    for (const auto& n : Nodes)
        if (n.Id == nodeId)
            return &n;
    return nullptr;
}

Edge* Model::FindLink(const std::string& linkId)
{
    for (auto& l : Links)
        if (l.Id == linkId)
            return &l;
    return nullptr;
}

bool Model::Validate() const
{
    auto hasPort = [](const Node& node, std::string_view portId) {
        if (portId.empty())
            return false;
        for (const auto& port : node.Ports)
        {
            if (port.Id == portId)
                return true;
        }
        return false;
    };

    // Shader-graph documents address a node's single output as "value", "out"
    // or "result" interchangeably (SgGraphCompiler registers all three), so a
    // link naming any alias is valid whenever the node has exactly one output.
    auto isOutputAlias = [](const std::string& portId) {
        return portId == "value" || portId == "out" || portId == "result";
    };
    auto hasSingleOutputPort = [](const Node& node) {
        std::size_t outputs = 0;
        for (const auto& port : node.Ports)
        {
            if (port.Direction == PortDirection::Out)
                ++outputs;
        }
        return outputs == 1;
    };

    std::unordered_set<std::string> linkKeys;
    for (const auto& link : Links)
    {
        std::string key = link.SourceNodeId + "|" + link.SourcePortId + "->" + link.TargetNodeId + "|" + link.TargetPortId;
        if (linkKeys.count(key))
            return false;
        linkKeys.insert(key);
        const Node* source = FindNode(link.SourceNodeId);
        const Node* target = FindNode(link.TargetNodeId);
        if (!source || !target)
            return false;
        // A node with no declared ports (tag-parsed, not yet hydrated from the
        // node registry) offers nothing to check a link's port id against.
        const bool sourceOk = source->Ports.empty() || hasPort(*source, link.SourcePortId) ||
                              (isOutputAlias(link.SourcePortId) && hasSingleOutputPort(*source));
        const bool targetOk = target->Ports.empty() || hasPort(*target, link.TargetPortId);
        if (!sourceOk || !targetOk)
            return false;
    }
    return true;
}

namespace {

using nlohmann::json;

constexpr const char* kModelKnownKeys[] = {
    "version", "kind", "lightingModel", "variantDefines", "viewport",
    "nodes", "variables", "textures", "links", "extensions"
};
constexpr const char* kNodeKnownKeys[] = {
    "id", "typeId", "positionX", "positionY", "pins", "parameters", "extensions"
};
constexpr const char* kPortKnownKeys[] = {
    "id", "direction", "dataType", "displayName"
};
constexpr const char* kEdgeKnownKeys[] = {
    "id", "sourceNodeId", "sourcePinId", "targetNodeId", "targetPinId"
};

bool IsKnownKey(const char* const* keys, std::size_t count, std::string_view key)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (key == keys[i])
            return true;
    }
    return false;
}

json GraphValueToJson(const GraphValue& value);
GraphValue GraphValueFromJson(const json& j);

json GraphObjectToJson(const GraphObject& object)
{
    json out = json::object();
    for (const auto& [key, value] : object)
        out[key] = GraphValueToJson(value);
    return out;
}

GraphObject GraphObjectFromJson(const json& j)
{
    GraphObject object;
    if (!j.is_object())
        return object;
    for (auto it = j.begin(); it != j.end(); ++it)
        object[it.key()] = GraphValueFromJson(*it);
    return object;
}

json GraphValueToJson(const GraphValue& value)
{
    switch (value.Kind())
    {
    case ValueKind::Null:
        return nullptr;
    case ValueKind::Bool:
        return value.AsBool();
    case ValueKind::Int:
        return value.AsInt();
    case ValueKind::Float:
        return value.AsFloat();
    case ValueKind::String:
    case ValueKind::Guid:
        return value.ToString();
    case ValueKind::List:
    {
        json array = json::array();
        if (const auto* list = value.TryList())
        {
            for (const GraphValue& item : *list)
                array.push_back(GraphValueToJson(item));
        }
        return array;
    }
    case ValueKind::Object:
        if (const GraphObject* object = value.TryObject())
            return GraphObjectToJson(*object);
        return json::object();
    }
    return nullptr;
}

GraphValue GraphValueFromJson(const json& j)
{
    if (j.is_null())
        return {};
    if (j.is_boolean())
        return GraphValue(j.get<bool>());
    if (j.is_number_unsigned())
    {
        const auto unsignedValue = j.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(INT64_MAX))
            return GraphValue(static_cast<double>(unsignedValue));
        return GraphValue(static_cast<std::int64_t>(unsignedValue));
    }
    if (j.is_number_integer())
        return GraphValue(j.get<std::int64_t>());
    if (j.is_number_float())
        return GraphValue(j.get<double>());
    if (j.is_string())
        return GraphValue(j.get<std::string>());
    if (j.is_array())
    {
        std::vector<GraphValue> list;
        list.reserve(j.size());
        for (const json& item : j)
            list.push_back(GraphValueFromJson(item));
        return GraphValue(std::move(list));
    }
    if (j.is_object())
        return GraphValue(GraphObjectFromJson(j));
    return {};
}

void MergePassthrough(json& dest, const GraphObject& passthrough)
{
    for (const auto& [key, value] : passthrough)
    {
        if (!dest.contains(key))
            dest[key] = GraphValueToJson(value);
    }
}

GraphObject CollectPassthrough(const json& j, const char* const* knownKeys, std::size_t knownCount)
{
    GraphObject passthrough;
    if (!j.is_object())
        return passthrough;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (IsKnownKey(knownKeys, knownCount, it.key()))
            continue;
        passthrough[it.key()] = GraphValueFromJson(*it);
    }
    return passthrough;
}

json PortToJson(const Port& port)
{
    json p = {
        {"id", port.Id},
        {"direction", PortDirectionToString(port.Direction)},
        {"dataType", port.DataType},
        {"displayName", port.DisplayName}
    };
    MergePassthrough(p, port.Passthrough);
    return p;
}

Port PortFromJson(const json& p)
{
    Port port;
    port.Id = p.value("id", "");
    port.Direction = PortDirectionFromString(p.value("direction", "in"));
    port.DataType = p.value("dataType", "");
    port.DisplayName = p.value("displayName", "");
    port.Passthrough = CollectPassthrough(p, kPortKnownKeys, std::size(kPortKnownKeys));
    return port;
}

json NodeToJson(const Node& node)
{
    json n;
    n["id"] = node.Id;
    n["typeId"] = node.TypeId;
    n["positionX"] = node.PositionX;
    n["positionY"] = node.PositionY;
    n["pins"] = json::array();
    for (const auto& port : node.Ports)
        n["pins"].push_back(PortToJson(port));
    if (!node.Parameters.empty())
        n["parameters"] = GraphObjectToJson(node.Parameters);
    if (!node.Extensions.empty())
        n["extensions"] = GraphObjectToJson(node.Extensions);
    MergePassthrough(n, node.Passthrough);
    return n;
}

Node NodeFromJson(const json& n)
{
    Node node;
    node.Id = n.value("id", "");
    node.TypeId = n.value("typeId", "");
    node.PositionX = n.value("positionX", 0.f);
    node.PositionY = n.value("positionY", 0.f);
    if (n.contains("pins") && n["pins"].is_array())
    {
        for (const auto& p : n["pins"])
            node.Ports.push_back(PortFromJson(p));
    }
    if (n.contains("parameters") && n["parameters"].is_object())
        node.Parameters = GraphObjectFromJson(n["parameters"]);
    if (n.contains("extensions") && n["extensions"].is_object())
        node.Extensions = GraphObjectFromJson(n["extensions"]);
    node.Passthrough = CollectPassthrough(n, kNodeKnownKeys, std::size(kNodeKnownKeys));
    return node;
}

json EdgeToJson(const Edge& link)
{
    json e = {
        {"id", link.Id},
        {"sourceNodeId", link.SourceNodeId},
        {"sourcePinId", link.SourcePortId},
        {"targetNodeId", link.TargetNodeId},
        {"targetPinId", link.TargetPortId}
    };
    MergePassthrough(e, link.Passthrough);
    return e;
}

Edge EdgeFromJson(const json& l)
{
    Edge link;
    link.Id = l.value("id", "");
    link.SourceNodeId = l.value("sourceNodeId", "");
    link.SourcePortId = l.value("sourcePinId", "");
    link.TargetNodeId = l.value("targetNodeId", "");
    link.TargetPortId = l.value("targetPinId", "");
    link.Passthrough = CollectPassthrough(l, kEdgeKnownKeys, std::size(kEdgeKnownKeys));
    return link;
}

} // namespace

std::string ToJson(const Model& model)
{
    using nlohmann::json;
    json j;
    j["version"] = kModelFormatVersion;
    j["kind"] = model.KindId;
    if (!model.LightingModel.empty())
        j["lightingModel"] = model.LightingModel;
    if (!model.VariantDefines.empty())
        j["variantDefines"] = model.VariantDefines;
    j["viewport"] = {
        {"panX", model.Viewport.PanX},
        {"panY", model.Viewport.PanY},
        {"zoom", model.Viewport.Zoom}
    };
    j["nodes"] = json::array();
    for (const auto& node : model.Nodes)
        j["nodes"].push_back(NodeToJson(node));
    j["variables"] = json::array();
    for (const auto& variable : model.Variables)
    {
        if (variable.Name.empty())
            continue;
        json v;
        v["name"] = variable.Name;
        v["type"] = variable.Type;
        v["value"] = variable.Value;
        v["public"] = variable.IsPublic;
        v["global"] = variable.IsGlobal;
        v["createdOrder"] = variable.CreatedOrder;
        if (!variable.RangeMin.empty())
            v["rangeMin"] = variable.RangeMin;
        if (!variable.RangeMax.empty())
            v["rangeMax"] = variable.RangeMax;
        if (!variable.Hint.empty())
            v["hint"] = variable.Hint;
        j["variables"].push_back(std::move(v));
    }
    j["textures"] = json::array();
    for (const auto& texture : model.Textures)
    {
        if (texture.Name.empty())
            continue;
        json t;
        t["name"] = texture.Name;
        t["guid"] = texture.Guid;
        if (!texture.Hint.empty())
            t["hint"] = texture.Hint;
        j["textures"].push_back(std::move(t));
    }
    j["links"] = json::array();
    for (const auto& link : model.Links)
        j["links"].push_back(EdgeToJson(link));
    if (!model.Extensions.empty())
        j["extensions"] = GraphObjectToJson(model.Extensions);
    MergePassthrough(j, model.Passthrough);
    return j.dump(2);
}

bool FromJson(const std::string& jsonStr, Model& outModel)
{
    using nlohmann::json;
    try
    {
        const json j = json::parse(jsonStr);
        // Parse into a local model and assign only on success: a failed load
        // must never leave the caller's model half-applied.
        Model model;
        model.Version = j.value("version", 1);
        model.KindId = j.value("kind", std::string(kKindIdGameLogic));
        model.LightingModel = j.value("lightingModel", "");
        model.VariantDefines = j.value("variantDefines", std::vector<std::string>{});
        if (j.contains("viewport") && j["viewport"].is_object())
        {
            const auto& v = j["viewport"];
            model.Viewport.PanX = v.value("panX", 0.f);
            model.Viewport.PanY = v.value("panY", 0.f);
            model.Viewport.Zoom = v.value("zoom", 1.f);
        }
        if (j.contains("nodes") && j["nodes"].is_array())
        {
            for (const auto& n : j["nodes"])
                model.Nodes.push_back(NodeFromJson(n));
        }
        if (j.contains("variables") && j["variables"].is_array())
        {
            std::uint64_t fallbackCreatedOrder = 0;
            for (const auto& v : j["variables"])
            {
                Variable variable;
                variable.Name = v.value("name", "");
                variable.Type = v.value("type", "float");
                variable.Value = v.contains("value") ? v.value("value", "") : DefaultVariableValueForType(variable.Type);
                variable.IsPublic = v.value("public", false);
                variable.IsGlobal = v.value("global", false);
                variable.CreatedOrder = v.value("createdOrder", fallbackCreatedOrder);
                variable.RangeMin = v.value("rangeMin", "");
                variable.RangeMax = v.value("rangeMax", "");
                variable.Hint = v.value("hint", "");
                ++fallbackCreatedOrder;
                if (!variable.Name.empty())
                    model.Variables.push_back(variable);
            }
        }
        if (j.contains("textures") && j["textures"].is_array())
        {
            for (const auto& t : j["textures"])
            {
                Texture texture;
                texture.Name = t.value("name", "");
                texture.Guid = t.value("guid", "");
                texture.Hint = t.value("hint", "");
                if (!texture.Name.empty())
                    model.Textures.push_back(texture);
            }
        }
        if (j.contains("links") && j["links"].is_array())
        {
            for (const auto& l : j["links"])
                model.Links.push_back(EdgeFromJson(l));
        }
        if (j.contains("extensions") && j["extensions"].is_object())
            model.Extensions = GraphObjectFromJson(j["extensions"]);
        model.Passthrough = CollectPassthrough(j, kModelKnownKeys, std::size(kModelKnownKeys));
        if (!model.Validate())
            return false;
        outModel = std::move(model);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace Graph
} // namespace GameEngine

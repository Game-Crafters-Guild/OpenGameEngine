#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphNodeIconStems.h"

#include "Graph/GameLogicActionCatalog.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace GameEngine {

namespace {

Graph::GraphValue GenericDefaultForPortType(const std::string& dataType)
{
    if (dataType == "float2")
        return Graph::GraphValue("vec2(0.0)");
    if (dataType == "float3")
        return Graph::GraphValue("vec3(0.0)");
    if (dataType == "float4")
        return Graph::GraphValue("vec4(0.0)");
    if (dataType == "bool")
        return Graph::GraphValue(false);
    return Graph::GraphValue(0.0f);
}

void Put(std::unordered_map<std::string, Graph::GraphValue>& out, std::string key,
         Graph::GraphValue value)
{
    out.emplace(std::move(key), std::move(value));
}

/* Material node glyph stems under editor:Icons/GraphNodes/. Types absent here
   (EngineInput, SurfaceOutput, VertexOutput) render without an icon. */
/* Keyed by the first path segment of the palette category. */
/* Game-logic node glyph stems; every catalog action has one. */
constexpr NodeIconStemEntry kGameLogicNodeIconStems[] = {
    {"AddForce", "addforce"},
    {"AnyState", "state"},
    {"BoolNot", "negate"},
    {"BoolOperator", "compare"},
    {"Branch", "if"},
    {"CollisionEvent", "collisionevent"},
    {"CompareBool", "compare"},
    {"CompareFloat", "compare"},
    {"CompareInt", "compare"},
    {"CreateEntity", "organization"},
    {"Delay", "time"},
    {"DestroyEntity", "organization"},
    {"DrawDebugLine", "debug-line"},
    {"Entry", "entry"},
    {"FindEntityByName", "findentity"},
    {"Finish", "output"},
    {"FloatAbs", "abs"},
    {"FloatClamp", "clamp"},
    {"FloatLerp", "lerp"},
    {"FloatOperator", "add"},
    {"FloatRound", "floor"},
    {"FloatSign", "sign"},
    {"GetDeltaTime", "time"},
    {"GetInputAction", "input-action"},
    {"GetInputAxis", "input-axis"},
    {"GetPosition", "position"},
    {"GetTime", "time"},
    {"GetVariable", "parameter"},
    {"GetVector3XYZ", "decompose"},
    {"HidePanel", "hidepanel"},
    {"IntOperator", "add"},
    {"LoadScene", "loadscene"},
    {"Log", "log"},
    {"LookAt", "ik"},
    {"MakeVector3", "compose"},
    {"MoveTo", "moveto"},
    {"PlaySound", "category-audio"},
    {"QuitApplication", "organization"},
    {"RandomFloat", "random"},
    {"RandomInt", "random"},
    {"Raycast", "raycast"},
    {"ReloadScene", "reloadscene"},
    {"Rotate", "rotate"},
    {"SendEvent", "reroute"},
    {"Sequence", "reroute"},
    {"SetBusVolume", "category-audio"},
    {"SetCameraActive", "camera"},
    {"SetEntityEnabled", "setenabled"},
    {"SetLightIntensity", "setlight"},
    {"SetPosition", "position"},
    {"SetSkyTimeOfDay", "time"},
    {"SetText", "settext"},
    {"SetTimeScale", "time"},
    {"SetVariable", "parameter"},
    {"SetVelocity", "setvelocity"},
    {"ShowPanel", "category-ui"},
    {"State", "state"},
    {"StopAllSounds", "category-audio"},
    {"StopMove", "stopmove"},
    {"ThirdPersonCameraFollow", "camerafollow"},
    {"Transition", "reroute"},
    {"Translate", "transform-pos"},
    {"TriggerEvent", "triggerevent"},
    {"Vector3Cross", "cross"},
    {"Vector3Distance", "distance"},
    {"Vector3Dot", "dot"},
    {"Vector3Lerp", "lerp"},
    {"Vector3Magnitude", "length"},
    {"Vector3Normalize", "normalize"},
    {"Vector3Operator", "add"},
    {"Vector3Scale", "multiply"},
};

constexpr NodeIconStemEntry kGameLogicCategoryIconStems[] = {
    {"Application", "category-utility"},
    {"Audio", "category-audio"},
    {"Debug", "category-debug"},
    {"Flow", "category-flow"},
    {"Game Object", "category-gameobject"},
    {"Input", "category-input"},
    {"Logic", "category-logic"},
    {"Math", "category-math"},
    {"Navigation", "category-navigation"},
    {"Physics", "category-physics"},
    {"Rendering", "category-rendering"},
    {"Scene", "category-scene"},
    {"Sky", "category-sky"},
    {"State Machine", "category-state"},
    {"Time", "category-time"},
    {"Transform", "category-transform"},
    {"UI", "category-ui"},
    {"Variables", "category-parameters"},
    {"Vector3", "category-vector"},
};

} // namespace

std::optional<NodeValueRange> FindNodeValueRange(const NodeTypeMeta* meta, std::string_view id)
{
    if (!meta)
        return std::nullopt;
    for (const NodeParamSpec& param : meta->Parameters)
    {
        if (param.Id == id)
        {
            if (param.Min && param.Max)
                return NodeValueRange{*param.Min, *param.Max};
            break;
        }
    }
    for (const NodePortTemplate& port : meta->Ports)
    {
        if (port.Direction == Graph::PortDirection::In && port.Id == id)
        {
            if (port.Min && port.Max)
                return NodeValueRange{*port.Min, *port.Max};
            break;
        }
    }
    return std::nullopt;
}

GraphNodeRegistry& GraphNodeRegistry::Get()
{
    static GraphNodeRegistry s_Registry;
    return s_Registry;
}

GraphNodeRegistry::GraphNodeRegistry()
{
    RegisterGameLogicTypes();
    RegisterMaterialTypes();
    RegisterAnimationTypes();
}

void GraphNodeRegistry::RegisterCategoryIcon(std::string_view kindId, std::string_view category,
                                             std::string_view stem)
{
    if (kindId.empty() || category.empty() || stem.empty())
        return;
    m_CategoryIconStems[std::string(kindId)][std::string(category)] = std::string(stem);
}

std::string GraphNodeRegistry::CategoryIconStem(std::string_view kindId,
                                                std::string_view category) const
{
    const auto kindIt = m_CategoryIconStems.find(std::string(kindId));
    if (kindIt == m_CategoryIconStems.end())
        return {};
    /* Menus fold nested categories to their first path segment. */
    const std::string_view head = category.substr(0, category.find('/'));
    const auto it = kindIt->second.find(std::string(head));
    return it == kindIt->second.end() ? std::string{} : it->second;
}

void GraphNodeRegistry::SetMetaDecorator(std::string_view kindId, MetaDecorator decorator)
{
    m_MetaDecorators[std::string(kindId)] = std::move(decorator);
}

void GraphNodeRegistry::SetTypeAliasResolver(std::string_view kindId, TypeAliasResolver resolver)
{
    m_TypeAliasResolvers[std::string(kindId)] = std::move(resolver);
}

void GraphNodeRegistry::Register(std::string_view kindId, NodeTypeMeta meta)
{
    if (kindId.empty() || meta.TypeId.empty())
        return;
    // Whatever this kind adds to every type of its own -- the registry does not
    // know what, only that the kind was asked.
    auto decorator = m_MetaDecorators.find(std::string(kindId));
    if (decorator != m_MetaDecorators.end() && decorator->second)
        decorator->second(meta);
    m_ByKindId[std::string(kindId)][meta.TypeId] = std::move(meta);
}

const NodeTypeMeta* GraphNodeRegistry::Find(std::string_view kindId, const std::string& typeId) const
{
    auto itKind = m_ByKindId.find(std::string(kindId));
    if (itKind == m_ByKindId.end())
        return nullptr;
    auto it = itKind->second.find(typeId);
    if (it == itKind->second.end())
        return nullptr;
    return &it->second;
}

const NodeTypeMeta* GraphNodeRegistry::FindNodeMeta(std::string_view kindId, const std::string& typeId) const
{
    if (const NodeTypeMeta* meta = Find(kindId, typeId))
        return meta;

    // A miss is the kind's to explain: it owns the vocabulary, so it is the one
    // that knows whether this id is an alias for a type it does store.
    auto resolver = m_TypeAliasResolvers.find(std::string(kindId));
    if (resolver == m_TypeAliasResolvers.end() || !resolver->second)
        return nullptr;
    const std::string resolved = resolver->second(typeId);
    if (resolved.empty() || resolved == typeId)
        return nullptr;
    return Find(kindId, resolved);
}

void GraphNodeRegistry::HydrateGraphModelPorts(Graph::Model& model) const
{
    for (Graph::Node& node : model.Nodes)
    {
        if (!node.Ports.empty())
            continue;

        const NodeTypeMeta* meta = FindNodeMeta(model.KindId, node.TypeId);
        if (!meta)
            continue;

        node.Ports.reserve(meta->Ports.size());
        for (const NodePortTemplate& pt : meta->Ports)
        {
            Graph::Port port;
            port.Id = pt.Id;
            port.Direction = pt.Direction;
            port.DataType = pt.DataType;
            port.DisplayName = pt.DisplayName;
            node.Ports.push_back(std::move(port));
        }
    }
}

std::vector<NodeTypeMeta> GraphNodeRegistry::GetAllTypes(std::string_view kindId) const
{
    std::vector<NodeTypeMeta> out;
    auto itKind = m_ByKindId.find(std::string(kindId));
    if (itKind == m_ByKindId.end())
        return out;
    for (const auto& [typeId, meta] : itKind->second)
        out.push_back(meta);
    return out;
}

std::unordered_map<std::string, Graph::GraphValue>
GraphNodeRegistry::GetDefaultParameterValues(std::string_view kindId, const std::string& typeId) const
{
    std::unordered_map<std::string, Graph::GraphValue> out;
    const NodeTypeMeta* meta = Find(kindId, typeId);
    if (!meta && kindId == Graph::kKindIdGameLogic)
    {
        if (const GameLogicActionSpec* spec = FindGameLogicActionSpec(typeId))
        {
            for (const GameLogicActionParameter& param : spec->Parameters)
                Put(out, param.Id, Graph::GraphValue(param.DefaultValue));
        }
        return out;
    }
    if (!meta)
        return out;

    for (const NodeParamSpec& param : meta->Parameters)
        Put(out, param.Id, param.Default);

    for (const auto& port : meta->Ports)
    {
        if (port.Direction != Graph::PortDirection::In || port.DataType == "flow" ||
            port.DataType == "texture2d")
            continue;
        if (out.find(port.Id) == out.end())
            Put(out, port.Id, GenericDefaultForPortType(port.DataType));
    }
    return out;
}

void GraphNodeRegistry::ApplyDefaultParameters(std::string_view kindId, const std::string& typeId,
                                              Graph::GraphObject& parameters) const
{
    for (const auto& [key, value] : GetDefaultParameterValues(kindId, typeId))
        parameters.emplace(key, value);
}

std::vector<std::string> GraphNodeRegistry::GetCategories(std::string_view kindId) const
{
    std::vector<std::string> out;
    auto itKind = m_ByKindId.find(std::string(kindId));
    if (itKind == m_ByKindId.end())
        return out;
    std::unordered_map<std::string, bool> seen;
    for (const auto& [typeId, meta] : itKind->second)
    {
        if (meta.Category.empty())
            continue;
        if (seen.emplace(meta.Category, true).second)
            out.push_back(meta.Category);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NodeTypeMeta> GraphNodeRegistry::GetTypesInCategory(std::string_view kindId,
                                                              const std::string& category) const
{
    std::vector<NodeTypeMeta> out;
    auto itKind = m_ByKindId.find(std::string(kindId));
    if (itKind == m_ByKindId.end())
        return out;
    for (const auto& [typeId, meta] : itKind->second)
    {
        if (meta.Category == category)
            out.push_back(meta);
    }
    return out;
}

std::unordered_map<std::string, std::string>
GraphNodeRegistry::GetDefaultParameters(std::string_view kindId, const std::string& typeId) const
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [key, value] : GetDefaultParameterValues(kindId, typeId))
        out.emplace(key, value.ToString());
    return out;
}

Graph::Node GraphNodeRegistry::CreateNode(std::string_view kindId, const std::string& typeId,
                                       const std::string& nodeId, float positionX,
                                       float positionY) const
{
    const NodeTypeMeta* meta = Find(kindId, typeId);
    Graph::Node node;
    node.Id = nodeId;
    node.TypeId = typeId;
    node.PositionX = positionX;
    node.PositionY = positionY;
    if (meta)
    {
        for (const auto& pt : meta->Ports)
        {
            Graph::Port port;
            port.Id = pt.Id;
            port.Direction = pt.Direction;
            port.DataType = pt.DataType;
            port.DisplayName = pt.DisplayName;
            node.Ports.push_back(port);
        }
    }
    ApplyDefaultParameters(kindId, typeId, node.Parameters);
    return node;
}

void GraphNodeRegistry::RegisterGameLogicTypes()
{
    for (const GameLogicActionSpec& spec : GetGameLogicActionCatalog())
    {
        NodeTypeMeta meta;
        meta.TypeId = spec.TypeId;
        meta.DisplayName = spec.DisplayName;
        meta.Category = spec.Category;
        meta.Ports.reserve(spec.Ports.size());
        for (const Graph::Port& port : spec.Ports)
            meta.Ports.push_back({port.Id, port.Direction, port.DataType, port.DisplayName});
        meta.Parameters.reserve(spec.Parameters.size());
        for (const GameLogicActionParameter& param : spec.Parameters)
            meta.Parameters.push_back({param.Id, param.DefaultValue, param.Options});
        meta.IconStem = std::string(FindIconStem(kGameLogicNodeIconStems, meta.TypeId));
        Register(Graph::kKindIdGameLogic, std::move(meta));
    }
    for (const NodeIconStemEntry& entry : kGameLogicCategoryIconStems)
        RegisterCategoryIcon(Graph::kKindIdGameLogic, entry.TypeId, entry.Stem);
}

} // namespace GameEngine

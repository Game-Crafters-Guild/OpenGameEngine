#include "Graph/GraphInspectorRows.h"

#include "Graph/GraphTypeRegistry.h"
#include "UI/Controls/FloatField.h"

#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <utility>

namespace GameEngine {
namespace {

bool HasKey(const std::unordered_map<std::string, std::string>& parameters, const char* key)
{
    return parameters.find(key) != parameters.end();
}

bool HasNonEmpty(const std::unordered_map<std::string, std::string>& parameters, const char* key)
{
    const auto it = parameters.find(key);
    return it != parameters.end() && !it->second.empty();
}

bool SchemaHas(const NodeTypeMeta* schema, std::string_view id)
{
    if (!schema)
        return false;
    for (const NodeParamSpec& param : schema->Parameters)
    {
        if (param.Id == id)
            return true;
    }
    return false;
}

bool IsMaterialParameterBinding(const NodeTypeMeta* schema,
                                const std::unordered_map<std::string, std::string>& parameters)
{
    // Material *Parameter nodes bind a graph variable and a shader slot/swizzle/component.
    // GetVariable / SetVariable declare variableName only. Extra live keys on a
    // schemed node do not reclassify it.
    if (schema)
    {
        if (!SchemaHas(schema, "variableName"))
            return false;
        return SchemaHas(schema, "slot") || SchemaHas(schema, "swizzle") || SchemaHas(schema, "component");
    }
    if (!HasKey(parameters, "variableName"))
        return false;
    return HasKey(parameters, "slot") || HasKey(parameters, "swizzle") || HasKey(parameters, "component");
}

bool IsMaterialInternalKey(std::string_view key)
{
    return key == "variableName" || key == "slot" || key == "component" || key == "swizzle";
}

bool IsRgbKey(std::string_view key)
{
    return key == "r" || key == "g" || key == "b";
}

bool IsVecKey(std::string_view key)
{
    return key == "x" || key == "y" || key == "z" || key == "w";
}

int CountVecAxes(const std::unordered_map<std::string, std::string>& parameters)
{
    if (!HasKey(parameters, "x") || !HasKey(parameters, "y"))
        return 0;
    int count = 2;
    if (HasKey(parameters, "z"))
        count = 3;
    if (HasKey(parameters, "w"))
        count = 4;
    return count;
}

bool TryParseIntParam(const std::string& text, int& out)
{
    if (text.empty())
        return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    if (end == begin)
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    out = static_cast<int>(value);
    return true;
}

GraphInspectorRowKind ScalarKindFromSchemaOrText(const NodeParamSpec* spec, const std::string& text)
{
    if (spec)
    {
        if (spec->Default.IsGuid() && spec->Id == "clipGuid")
            return GraphInspectorRowKind::NodeAsset;
        if (spec->Default.IsInt())
            return GraphInspectorRowKind::NodeInt;
        if (spec->Default.IsFloat())
            return GraphInspectorRowKind::NodeFloat;
    }

    int intValue = 0;
    float floatValue = 0.0f;
    if (TryParseIntParam(text, intValue))
        return GraphInspectorRowKind::NodeInt;
    if (FloatField::TryParseFloat(text, floatValue))
        return GraphInspectorRowKind::NodeFloat;
    return GraphInspectorRowKind::NodeText;
}

} // namespace

const NodeParamSpec* FindSchemaParam(const NodeTypeMeta* schema, std::string_view id)
{
    if (!schema)
        return nullptr;
    for (const NodeParamSpec& param : schema->Parameters)
    {
        if (param.Id == id)
            return &param;
    }
    return nullptr;
}

const NodeTypeMeta* FindUniqueNodeTypeMeta(std::string_view typeId)
{
    if (typeId.empty())
        return nullptr;

    const std::string type(typeId);
    const NodeTypeMeta* found = nullptr;
    for (const Graph::GraphTypeDesc& desc : Graph::GraphTypeRegistry::Get().All())
    {
        const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(desc.Id, type);
        if (!meta)
            continue;
        if (found)
            return nullptr;
        found = meta;
    }
    return found;
}

const NodeTypeMeta* FindNodeTypeMeta(std::string_view kindId, std::string_view typeId)
{
    if (!kindId.empty())
        return GraphNodeRegistry::Get().Find(kindId, std::string(typeId));
    return FindUniqueNodeTypeMeta(typeId);
}

std::vector<GraphInspectorRow> PlanGraphInspectorRows(
    const std::unordered_map<std::string, std::string>& parameters,
    const NodeTypeMeta* schema)
{
    std::vector<GraphInspectorRow> rows;

    const bool isMaterialParam = IsMaterialParameterBinding(schema, parameters);
    const bool linked = HasNonEmpty(parameters, "variableName");
    const bool hasRgb = HasKey(parameters, "r") && HasKey(parameters, "g") && HasKey(parameters, "b");
    const bool hasValue = HasKey(parameters, "value");
    const int vecCount = CountVecAxes(parameters);

    if (isMaterialParam && linked)
        rows.push_back({GraphInspectorRowKind::VariableName, "Variable", "variableName"});

    const bool emitColorSwatch = hasRgb && !isMaterialParam;
    const bool emitVariableColor = hasRgb && isMaterialParam && linked;
    if (emitColorSwatch)
        rows.push_back({GraphInspectorRowKind::ColorSwatch, "Color", "r"});
    if (emitVariableColor)
        rows.push_back({GraphInspectorRowKind::VariableColor, "Color", "variableName"});

    const bool emitVariableFloat =
        isMaterialParam && linked && !hasRgb && vecCount < 2 &&
        (hasValue || SchemaHas(schema, "component"));
    if (emitVariableFloat)
        rows.push_back({GraphInspectorRowKind::VariableFloat, "Value", "value"});

    if (vecCount >= 2 && isMaterialParam && linked)
    {
        static const char* kLabels[] = {"X", "Y", "Z", "W"};
        static const char* kKeys[] = {"x", "y", "z", "w"};
        for (int axis = 0; axis < vecCount; ++axis)
        {
            GraphInspectorRow row;
            row.Kind = GraphInspectorRowKind::VariableVec;
            row.Label = kLabels[axis];
            row.Key = kKeys[axis];
            row.VecAxis = axis;
            row.VecCount = vecCount;
            rows.push_back(std::move(row));
        }
    }

    const bool hideRgb = hasRgb && (emitColorSwatch || emitVariableColor || (isMaterialParam && !linked));
    const bool hideValue = isMaterialParam && hasValue;
    const bool hideVec = isMaterialParam && vecCount >= 2;

    std::unordered_set<std::string> consumed;
    auto skipKey = [&](std::string_view key) -> bool
    {
        if (isMaterialParam && IsMaterialInternalKey(key))
            return true;
        if (hideRgb && IsRgbKey(key))
            return true;
        if (hideValue && key == "value")
            return true;
        if (hideVec && IsVecKey(key))
            return true;
        return false;
    };

    auto emitScalar = [&](const std::string& key, const std::string& value, const NodeParamSpec* spec)
    {
        if (consumed.contains(key) || skipKey(key))
            return;
        consumed.insert(key);
        GraphInspectorRow row;
        row.Kind = (spec && !spec->Options.empty()) ? GraphInspectorRowKind::NodeEnum
                                                    : ScalarKindFromSchemaOrText(spec, value);
        row.Key = key;
        row.Label = (row.Kind == GraphInspectorRowKind::NodeAsset) ? "Clip" : key;
        rows.push_back(std::move(row));
    };

    if (schema)
    {
        for (const NodeParamSpec& param : schema->Parameters)
        {
            const auto it = parameters.find(param.Id);
            if (it == parameters.end())
                continue;
            emitScalar(param.Id, it->second, &param);
        }
    }
    for (const auto& [key, value] : parameters)
        emitScalar(key, value, FindSchemaParam(schema, key));

    return rows;
}

} // namespace GameEngine

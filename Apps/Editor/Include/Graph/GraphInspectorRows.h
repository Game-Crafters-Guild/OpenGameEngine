#pragma once

#include "Graph/GraphNodeRegistry.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine {

enum class GraphInspectorRowKind
{
    VariableName,
    ColorSwatch,
    VariableColor,
    VariableFloat,
    VariableVec,
    NodeInt,
    NodeFloat,
    NodeText,
    NodeEnum,
    NodeAsset
};

struct GraphInspectorRow
{
    GraphInspectorRowKind Kind = GraphInspectorRowKind::NodeText;
    std::string Label;
    std::string Key;
    int VecAxis = -1;
    int VecCount = 0;
};

/** Null when `typeId` is missing or registered under more than one kind. */
const NodeTypeMeta* FindUniqueNodeTypeMeta(std::string_view typeId);
/** The schema's spec for one parameter id; null without a schema or a match. */
const NodeParamSpec* FindSchemaParam(const NodeTypeMeta* schema, std::string_view id);
/** KindId lookup when the open graph is known; otherwise FindUnique. */
const NodeTypeMeta* FindNodeTypeMeta(std::string_view kindId, std::string_view typeId);

std::vector<GraphInspectorRow> PlanGraphInspectorRows(
    const std::unordered_map<std::string, std::string>& parameters,
    const NodeTypeMeta* schema);

} // namespace GameEngine

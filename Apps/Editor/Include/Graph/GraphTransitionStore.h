#pragma once

#include "Graph/GraphModel.h"
#include "Graph/GraphValue.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {

struct GraphTransitionConditionDesc
{
    std::string Param;
    std::string Op{"equals"};
    Graph::GraphValue Value{0.0f};
};

struct GraphTransitionDesc
{
    float Duration = 0.2f;
    std::vector<GraphTransitionConditionDesc> Conditions;
};

struct GraphTransitionCompareOp
{
    const char* Value;
    const char* Label;
};

inline constexpr GraphTransitionCompareOp kGraphTransitionCompareOps[] = {
    {"equals", "Equals"},
    {"notEquals", "Not Equals"},
    {"greaterThan", "Greater Than"},
    {"lessThan", "Less Than"},
    {"greaterOrEqual", "Greater or Equal"},
    {"lessOrEqual", "Less or Equal"},
};

class GraphTransitionStore
{
public:
    static constexpr float kDefaultDuration = 0.2f;

    static bool IsStateTransition(const Graph::Model& model, const Graph::Edge& link);
    static GraphTransitionDesc Load(const Graph::Edge& link);
    static void Store(Graph::Edge& link, const GraphTransitionDesc& desc);
    static bool TryParseValueText(std::string_view text, Graph::GraphValue& out);
    static std::string ValueToText(const Graph::GraphValue& value);
    static GraphTransitionConditionDesc MakeDefaultCondition();
};

} // namespace GameEngine

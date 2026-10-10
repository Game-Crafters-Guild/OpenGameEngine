#include "Graph/GraphTransitionStore.h"

#include "Animation/AnimParam.h"
#include "Types/ParseNumber.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <system_error>

namespace GameEngine {

namespace {

std::string_view TrimView(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool FloatNarrowsFinite(double n)
{
    return std::isfinite(n) && std::isfinite(static_cast<float>(n));
}

bool ValueIsStoreable(const Graph::GraphValue& value)
{
    if (value.IsBool())
        return true;
    if (value.IsInt())
    {
        const std::int64_t n = value.AsInt();
        return n >= std::numeric_limits<std::int32_t>::min() && n <= std::numeric_limits<std::int32_t>::max();
    }
    if (value.IsFloat())
        return FloatNarrowsFinite(value.AsFloat());
    return false;
}

} // namespace

bool GraphTransitionStore::IsStateTransition(const Graph::Model& model, const Graph::Edge& link)
{
    if (link.SourcePortId != "out" || link.TargetPortId != "in")
        return false;
    const Graph::Node* source = model.FindNode(link.SourceNodeId);
    const Graph::Node* target = model.FindNode(link.TargetNodeId);
    if (!source || !target)
        return false;
    return source->TypeId == "State" && target->TypeId == "State";
}

GraphTransitionDesc GraphTransitionStore::Load(const Graph::Edge& link)
{
    GraphTransitionDesc desc;
    const auto durIt = link.Passthrough.find("duration");
    if (durIt != link.Passthrough.end() && durIt->second.IsNumber())
    {
        const float duration = static_cast<float>(durIt->second.AsFloat());
        if (std::isfinite(duration) && duration >= 0.f)
            desc.Duration = duration;
    }

    const auto condIt = link.Passthrough.find("conditions");
    if (condIt == link.Passthrough.end())
        return desc;
    const std::vector<Graph::GraphValue>* list = condIt->second.TryList();
    if (!list)
        return desc;

    desc.Conditions.reserve(list->size());
    for (const Graph::GraphValue& entry : *list)
    {
        const Graph::GraphObject* obj = entry.TryObject();
        if (!obj)
            continue;
        const std::string param = obj->GetString("param");
        if (param.empty())
            continue;
        Animation::ParamCompare op = Animation::ParamCompare::Equals;
        if (!Animation::TryParamCompareFromName(obj->GetString("op", "equals"), op))
            continue;
        const auto valueIt = obj->find("value");
        if (valueIt == obj->end())
            continue;

        GraphTransitionConditionDesc cond;
        cond.Param = param;
        cond.Op = Animation::ParamCompareName(op);
        if (valueIt->second.IsBool() || valueIt->second.IsInt() || valueIt->second.IsFloat())
            cond.Value = valueIt->second;
        else
            continue;
        desc.Conditions.push_back(std::move(cond));
    }
    return desc;
}

void GraphTransitionStore::Store(Graph::Edge& link, const GraphTransitionDesc& desc)
{
    float duration = desc.Duration;
    if (!std::isfinite(duration) || duration < 0.f)
        duration = kDefaultDuration;
    link.Passthrough["duration"] = duration;

    std::vector<Graph::GraphValue> list;
    list.reserve(desc.Conditions.size());
    for (const GraphTransitionConditionDesc& cond : desc.Conditions)
    {
        if (cond.Param.empty())
            continue;
        Animation::ParamCompare op = Animation::ParamCompare::Equals;
        if (!Animation::TryParamCompareFromName(cond.Op, op))
            continue;
        if (!ValueIsStoreable(cond.Value))
            continue;
        Graph::GraphObject obj;
        obj["param"] = cond.Param;
        obj["op"] = Animation::ParamCompareName(op);
        obj["value"] = cond.Value;
        list.emplace_back(Graph::GraphValue(std::move(obj)));
    }
    link.Passthrough["conditions"] = Graph::GraphValue(std::move(list));
}

bool GraphTransitionStore::TryParseValueText(std::string_view text, Graph::GraphValue& out)
{
    text = TrimView(text);
    if (text.empty())
        return false;
    if (text == "true" || text == "True" || text == "TRUE")
    {
        out = true;
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE")
    {
        out = false;
        return true;
    }

    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const bool scientific = text.find('e') != std::string_view::npos || text.find('E') != std::string_view::npos;
    const bool decimal = text.find('.') != std::string_view::npos;
    if (!scientific && !decimal)
    {
        std::int64_t n = 0;
        const std::from_chars_result parsed = std::from_chars(begin, end, n);
        if (parsed.ec == std::errc{} && parsed.ptr == end)
        {
            if (n < std::numeric_limits<std::int32_t>::min() || n > std::numeric_limits<std::int32_t>::max())
                return false;
            out = static_cast<int>(n);
            return true;
        }
    }

    // Apple Clang has no floating-point std::from_chars; ParseNumber.h is the
    // portable path and keeps the same fail-close semantics on overflow.
    std::size_t consumed = 0;
    const std::optional<double> parsed = ParseDouble(text, &consumed);
    if (!parsed || consumed != text.size() || !FloatNarrowsFinite(*parsed))
        return false;
    out = static_cast<float>(*parsed);
    return true;
}

std::string GraphTransitionStore::ValueToText(const Graph::GraphValue& value)
{
    if (value.IsBool())
        return value.AsBool() ? "true" : "false";
    if (value.IsInt())
        return std::to_string(value.AsInt());
    if (value.IsFloat())
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", value.AsFloat());
        return buf;
    }
    return value.ToString();
}

GraphTransitionConditionDesc GraphTransitionStore::MakeDefaultCondition()
{
    GraphTransitionConditionDesc cond;
    cond.Param = "Speed";
    cond.Op = "greaterThan";
    cond.Value = 0.0f;
    return cond;
}

} // namespace GameEngine

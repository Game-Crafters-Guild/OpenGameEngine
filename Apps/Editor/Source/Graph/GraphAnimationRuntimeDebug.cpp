#include "Graph/GraphAnimationRuntimeDebug.h"

#include "Types/StringId.h"

#include <cmath>

namespace GameEngine {

std::string GraphAnimationRuntimeDebug::FindStateNodeId(const Graph::Model& nested,
                                                        std::string_view stateName)
{
    if (stateName.empty())
        return {};
    for (const Graph::Node& node : nested.Nodes)
    {
        if (node.TypeId != "State")
            continue;
        const std::string title = node.Parameters.GetString("title");
        const std::string_view match = title.empty() ? std::string_view(node.Id) : std::string_view(title);
        if (match == stateName)
            return node.Id;
    }
    return {};
}

GraphAnimationRuntimeHighlight GraphAnimationRuntimeDebug::ForStateMachine(
    const Graph::Model& nested, const Animation::StateMachineNode& sm)
{
    GraphAnimationRuntimeHighlight highlight;
    const auto& states = sm.GetStates();
    const uint32_t active = sm.GetActiveStateIndex();
    if (active >= states.size())
        return highlight;

    const std::string fromId = FindStateNodeId(nested, states[active].Name);
    if (!fromId.empty())
        highlight.NodeIds.insert(fromId);

    if (!sm.IsTransitioning())
        return highlight;

    const uint32_t target = sm.GetTransitionTargetIndex();
    if (target >= states.size())
        return highlight;
    const std::string toId = FindStateNodeId(nested, states[target].Name);
    if (!toId.empty())
        highlight.NodeIds.insert(toId);
    if (fromId.empty() || toId.empty())
        return highlight;

    for (const Graph::Edge& link : nested.Links)
    {
        if (link.SourcePortId != "out" || link.TargetPortId != "in")
            continue;
        if (link.SourceNodeId == fromId && link.TargetNodeId == toId)
        {
            highlight.LinkIds.push_back(link.Id);
            break;
        }
    }
    return highlight;
}

bool GraphAnimationRuntimeDebug::StateIsEvaluating(const Graph::Model& stateMachineNested,
                                                   std::string_view stateNodeId,
                                                   const Animation::StateMachineNode& sm)
{
    const Graph::Node* host = stateMachineNested.FindNode(std::string(stateNodeId));
    if (!host || host->TypeId != "State")
        return false;
    const std::string title = host->Parameters.GetString("title");
    const std::string_view name = title.empty() ? std::string_view(host->Id) : std::string_view(title);
    const auto& states = sm.GetStates();
    auto matches = [&](uint32_t index)
    {
        return index < states.size() && states[index].Name == name;
    };
    if (matches(sm.GetActiveStateIndex()))
        return true;
    return sm.IsTransitioning() && matches(sm.GetTransitionTargetIndex());
}

GraphAnimationRuntimeHighlight GraphAnimationRuntimeDebug::ForPoseGraph(const Graph::Model& nested)
{
    GraphAnimationRuntimeHighlight highlight;
    std::unordered_set<std::string> outputIds;
    for (const Graph::Node& node : nested.Nodes)
    {
        if (node.TypeId != "OutputPose")
            continue;
        highlight.NodeIds.insert(node.Id);
        outputIds.insert(node.Id);
    }
    for (const Graph::Edge& link : nested.Links)
    {
        if (link.TargetPortId != "pose" || outputIds.count(link.TargetNodeId) == 0)
            continue;
        if (!link.SourceNodeId.empty())
            highlight.NodeIds.insert(link.SourceNodeId);
    }
    return highlight;
}

bool GraphAnimationRuntimeDebug::TryPreviewFloat(const Animation::AnimationGraphPlayer& player,
                                                 std::string_view name, float& out)
{
    if (name.empty())
        return false;
    float value = 0.f;
    if (!player.TryGetFloat(HashStringId(name), value) || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

} // namespace GameEngine

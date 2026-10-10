#include "Graph/GraphFsmRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace GameEngine {

namespace {

constexpr float kMinTimerSeconds = 0.001f;
constexpr int kMaxTransitionsPerTick = 8;

bool IsStateNode(const Graph::Node& node)
{
    return node.TypeId == "State";
}

bool IsAnyStateNode(const Graph::Node& node)
{
    return node.TypeId == "AnyState";
}

bool IsTransitionNode(const Graph::Node& node)
{
    return node.TypeId == "Transition";
}

bool IsEntryNode(const Graph::Node& node)
{
    return node.TypeId == "Entry";
}

float ParseFloatOrDefault(const Graph::GraphObject& parameters,
                          const std::string& key, float fallback)
{
    return static_cast<float>(parameters.GetFloat(key, fallback));
}

bool ParseBoolOrDefault(const Graph::GraphObject& parameters,
                        const std::string& key, bool fallback)
{
    return parameters.GetBool(key, fallback);
}

GraphFsmConditionKind ParseConditionKind(const Graph::GraphObject& parameters)
{
    const std::string mode = parameters.GetString("mode");
    std::string lowered = mode;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "timer")
        return GraphFsmConditionKind::Timer;
    if (lowered == "event")
        return GraphFsmConditionKind::Event;
    if (lowered == "bool")
        return GraphFsmConditionKind::Bool;
    return GraphFsmConditionKind::Always;
}

} // namespace

bool GraphFsmRuntime::Build(const Graph::Model& graph, std::string& outError)
{
    outError.clear();
    m_Definition = {};
    Reset();

    if (graph.KindId != Graph::kKindIdGameLogic)
    {
        outError = "Graph FSM runtime supports only GameLogic graphs.";
        return false;
    }

    std::unordered_map<std::string, const Graph::Node*> nodeById;
    nodeById.reserve(graph.Nodes.size());
    for (const auto& node : graph.Nodes)
        nodeById[node.Id] = &node;

    for (const auto& node : graph.Nodes)
    {
        if (!IsStateNode(node))
            continue;
        GraphFsmState state;
        state.NodeId = node.Id;
        m_Definition.States.emplace(state.NodeId, std::move(state));
    }

    if (m_Definition.States.empty())
    {
        outError = "No State nodes found in graph.";
        return false;
    }

    for (const auto& node : graph.Nodes)
    {
        if (!IsTransitionNode(node))
            continue;

        std::vector<const Graph::Edge*> incoming;
        std::vector<const Graph::Edge*> outgoing;
        for (const auto& link : graph.Links)
        {
            if (link.TargetNodeId == node.Id)
                incoming.push_back(&link);
            if (link.SourceNodeId == node.Id)
                outgoing.push_back(&link);
        }
        if (outgoing.empty())
            continue;

        std::vector<std::string> sourceStateIds;
        for (const auto* link : incoming)
        {
            auto sourceIt = nodeById.find(link->SourceNodeId);
            if (sourceIt == nodeById.end())
                continue;
            const Graph::Node* sourceNode = sourceIt->second;
            if (IsStateNode(*sourceNode))
            {
                sourceStateIds.push_back(sourceNode->Id);
            }
            else if (IsAnyStateNode(*sourceNode))
            {
                sourceStateIds.reserve(sourceStateIds.size() + m_Definition.States.size());
                for (const auto& [stateId, _state] : m_Definition.States)
                    sourceStateIds.push_back(stateId);
            }
        }
        if (sourceStateIds.empty())
            continue;

        for (const auto* outLink : outgoing)
        {
            auto targetIt = nodeById.find(outLink->TargetNodeId);
            if (targetIt == nodeById.end())
                continue;
            const Graph::Node* targetNode = targetIt->second;
            if (!IsStateNode(*targetNode))
                continue;

            for (const std::string& sourceStateId : sourceStateIds)
            {
                if (sourceStateId == targetNode->Id)
                    continue;
                auto stateIt = m_Definition.States.find(sourceStateId);
                if (stateIt == m_Definition.States.end())
                    continue;

                GraphFsmTransition transition;
                transition.SourceStateNodeId = sourceStateId;
                transition.TransitionNodeId = node.Id;
                transition.TargetStateNodeId = targetNode->Id;
                transition.Condition = ParseConditionKind(node.Parameters);
                transition.TimerSeconds = std::max(
                    kMinTimerSeconds, ParseFloatOrDefault(node.Parameters, "seconds", 0.5f));
                auto eventIt = node.Parameters.find("event");
                if (eventIt != node.Parameters.end())
                    transition.EventName = eventIt->second.ToString();
                auto boolVarIt = node.Parameters.find("boolVar");
                if (boolVarIt != node.Parameters.end())
                    transition.BoolVariableName = boolVarIt->second.ToString();
                transition.BoolExpectedValue = ParseBoolOrDefault(node.Parameters, "boolValue", true);
                transition.VisualLinkIds.push_back(outLink->Id);
                for (const auto* inLink : incoming)
                {
                    if (inLink->SourceNodeId == sourceStateId)
                    {
                        transition.VisualLinkIds.push_back(inLink->Id);
                        continue;
                    }
                    auto visualSourceIt = nodeById.find(inLink->SourceNodeId);
                    if (visualSourceIt != nodeById.end() && IsAnyStateNode(*visualSourceIt->second))
                        transition.VisualLinkIds.push_back(inLink->Id);
                }

                stateIt->second.OutgoingTransitions.push_back(std::move(transition));
            }
        }
    }

    for (const auto& link : graph.Links)
    {
        auto srcIt = nodeById.find(link.SourceNodeId);
        auto dstIt = nodeById.find(link.TargetNodeId);
        if (srcIt == nodeById.end() || dstIt == nodeById.end())
            continue;
        if (!IsStateNode(*srcIt->second) || !IsStateNode(*dstIt->second))
            continue;

        auto stateIt = m_Definition.States.find(srcIt->second->Id);
        if (stateIt == m_Definition.States.end())
            continue;
        GraphFsmTransition transition;
        transition.SourceStateNodeId = srcIt->second->Id;
        transition.TargetStateNodeId = dstIt->second->Id;
        transition.Condition = GraphFsmConditionKind::Always;
        transition.VisualLinkIds.push_back(link.Id);
        stateIt->second.OutgoingTransitions.push_back(std::move(transition));
    }

    for (const auto& node : graph.Nodes)
    {
        if (!IsEntryNode(node))
            continue;
        for (const auto& link : graph.Links)
        {
            if (link.SourceNodeId != node.Id)
                continue;
            if (m_Definition.States.count(link.TargetNodeId) != 0)
            {
                m_Definition.EntryStateNodeId = link.TargetNodeId;
                break;
            }
        }
        if (!m_Definition.EntryStateNodeId.empty())
            break;
    }

    if (m_Definition.EntryStateNodeId.empty())
    {
        m_Definition.EntryStateNodeId = m_Definition.States.begin()->first;
    }

    EnterState(m_Definition.EntryStateNodeId);
    return true;
}

void GraphFsmRuntime::Reset()
{
    m_ActiveStateNodeId.clear();
    m_ElapsedStateSeconds = 0.f;
    m_PendingEvents.clear();
    m_LastFiredTransitionLinkIds.clear();
    m_LastFiredSourceStateNodeId.clear();
    m_LastFiredTransitionNodeId.clear();
    m_LastFiredTargetStateNodeId.clear();
}

void GraphFsmRuntime::Tick(float deltaSeconds)
{
    m_LastFiredTransitionLinkIds.clear();
    m_LastFiredSourceStateNodeId.clear();
    m_LastFiredTransitionNodeId.clear();
    m_LastFiredTargetStateNodeId.clear();
    if (m_ActiveStateNodeId.empty())
        return;

    m_ElapsedStateSeconds += std::max(0.f, deltaSeconds);
    int transitionsTaken = 0;
    while (transitionsTaken < kMaxTransitionsPerTick)
    {
        auto stateIt = m_Definition.States.find(m_ActiveStateNodeId);
        if (stateIt == m_Definition.States.end())
            break;

        const GraphFsmTransition* firedTransition = nullptr;
        for (const auto& transition : stateIt->second.OutgoingTransitions)
        {
            if (IsTransitionSatisfied(transition))
            {
                firedTransition = &transition;
                break;
            }
        }
        if (!firedTransition)
            break;

        m_LastFiredTransitionLinkIds = firedTransition->VisualLinkIds;
        m_LastFiredSourceStateNodeId = firedTransition->SourceStateNodeId;
        m_LastFiredTransitionNodeId = firedTransition->TransitionNodeId;
        m_LastFiredTargetStateNodeId = firedTransition->TargetStateNodeId;
        EnterState(firedTransition->TargetStateNodeId);
        transitionsTaken++;
        if (firedTransition->Condition != GraphFsmConditionKind::Always)
            break;
    }
    m_PendingEvents.clear();
}

void GraphFsmRuntime::TriggerEvent(const std::string& eventName)
{
    if (!eventName.empty())
        m_PendingEvents.insert(eventName);
}

void GraphFsmRuntime::SetBoolVariable(const std::string& variableName, bool value)
{
    if (variableName.empty())
        return;
    m_BoolVariables[variableName] = value;
}

bool GraphFsmRuntime::IsTransitionSatisfied(const GraphFsmTransition& transition) const
{
    switch (transition.Condition)
    {
    case GraphFsmConditionKind::Always:
        return true;
    case GraphFsmConditionKind::Timer:
        return m_ElapsedStateSeconds >= transition.TimerSeconds;
    case GraphFsmConditionKind::Event:
        return !transition.EventName.empty() && m_PendingEvents.count(transition.EventName) != 0;
    case GraphFsmConditionKind::Bool:
    {
        if (transition.BoolVariableName.empty())
            return false;
        auto it = m_BoolVariables.find(transition.BoolVariableName);
        if (it == m_BoolVariables.end())
            return false;
        return it->second == transition.BoolExpectedValue;
    }
    }
    return false;
}

void GraphFsmRuntime::EnterState(const std::string& stateNodeId)
{
    if (m_Definition.States.count(stateNodeId) == 0)
        return;
    m_ActiveStateNodeId = stateNodeId;
    m_ElapsedStateSeconds = 0.f;
}

} // namespace GameEngine

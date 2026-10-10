#pragma once

#include "Graph/GraphModel.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine {

enum class GraphFsmConditionKind {
    Always,
    Timer,
    Event,
    Bool
};

struct GraphFsmTransition {
    std::string SourceStateNodeId;
    std::string TransitionNodeId;
    std::string TargetStateNodeId;
    GraphFsmConditionKind Condition = GraphFsmConditionKind::Always;
    float TimerSeconds = 0.f;
    std::string EventName;
    std::string BoolVariableName;
    bool BoolExpectedValue = true;
    std::vector<std::string> VisualLinkIds;
};

struct GraphFsmState {
    std::string NodeId;
    std::vector<GraphFsmTransition> OutgoingTransitions;
};

struct GraphFsmDefinition {
    std::string EntryStateNodeId;
    std::unordered_map<std::string, GraphFsmState> States;
};

class GraphFsmRuntime {
public:
    bool Build(const Graph::Model& graph, std::string& outError);
    void Reset();

    void Tick(float deltaSeconds);
    void TriggerEvent(const std::string& eventName);
    void SetBoolVariable(const std::string& variableName, bool value);

    bool HasDefinition() const { return !m_Definition.States.empty(); }
    const GraphFsmDefinition& GetDefinition() const { return m_Definition; }
    const std::string& GetActiveStateNodeId() const { return m_ActiveStateNodeId; }
    const std::vector<std::string>& GetLastFiredTransitionLinkIds() const { return m_LastFiredTransitionLinkIds; }
    /** Cleared each Tick; populated when a transition fires (for play-mode graph highlights). */
    const std::string& GetLastFiredSourceStateNodeId() const { return m_LastFiredSourceStateNodeId; }
    const std::string& GetLastFiredTransitionNodeId() const { return m_LastFiredTransitionNodeId; }
    const std::string& GetLastFiredTargetStateNodeId() const { return m_LastFiredTargetStateNodeId; }

private:
    bool IsTransitionSatisfied(const GraphFsmTransition& transition) const;
    void EnterState(const std::string& stateNodeId);

    GraphFsmDefinition m_Definition;
    std::string m_ActiveStateNodeId;
    float m_ElapsedStateSeconds = 0.f;
    std::unordered_set<std::string> m_PendingEvents;
    std::unordered_map<std::string, bool> m_BoolVariables;
    std::vector<std::string> m_LastFiredTransitionLinkIds;
    std::string m_LastFiredSourceStateNodeId;
    std::string m_LastFiredTransitionNodeId;
    std::string m_LastFiredTargetStateNodeId;
};

} // namespace GameEngine

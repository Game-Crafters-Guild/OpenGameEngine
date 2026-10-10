#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimParam.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// Defines a transition between two states.
struct StateTransition
{
    uint32_t TargetStateIndex = 0;
    float Duration = 0.2f; // crossfade duration in seconds
    std::vector<TransitionCondition> Conditions;
};

// A single state within the state machine, owning a child graph node.
struct StateMachineState
{
    std::string Name;
    std::unique_ptr<AnimGraphNode> Node;
    std::vector<StateTransition> Transitions;
};

// Evaluates a finite state machine over animation graph nodes.
// Each state owns a child node (typically a ClipPlayer or sub-graph).
// Transitions are checked each frame; when one fires, a linear crossfade
// blends the outgoing and incoming states over the transition duration.
class StateMachineNode : public AnimGraphNode
{
public:
    // Add a state and return its index.
    uint32_t AddState(std::string name, std::unique_ptr<AnimGraphNode> node);

    // Add a transition from one state to another.
    void AddTransition(uint32_t fromState, uint32_t toState, float duration,
                       std::vector<TransitionCondition> conditions);

    // Parameter access for driving transition conditions (StringId keys).
    void SetParam(std::string_view name, ParamValue value);
    void SetParam(::GameEngine::StringId id, ParamValue value);
    const ParamValue* GetParam(::GameEngine::StringId id) const;

    void SetActiveState(uint32_t stateIndex);
    uint32_t GetActiveStateIndex() const;
    bool IsTransitioning() const { return m_Transitioning; }
    uint32_t GetTransitionTargetIndex() const { return m_TransitionTarget; }
    const std::vector<StateMachineState>& GetStates() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    bool CheckConditions(const std::vector<TransitionCondition>& conditions,
                         const EvaluationContext& ctx) const;

    std::vector<StateMachineState> m_States;
    std::unordered_map<::GameEngine::StringId, ParamValue> m_Params;

    uint32_t m_ActiveState = 0;

    // Crossfade state
    bool m_Transitioning = false;
    uint32_t m_TransitionTarget = 0;
    float m_TransitionDuration = 0.0f;
    float m_TransitionElapsed = 0.0f;
};

} // namespace Animation
} // namespace GameEngine

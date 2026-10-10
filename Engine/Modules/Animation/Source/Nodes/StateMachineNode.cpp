#include "Animation/Nodes/StateMachineNode.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Types/StringId.h"

#include <algorithm>

namespace GameEngine
{
namespace Animation
{

uint32_t StateMachineNode::AddState(std::string name, std::unique_ptr<AnimGraphNode> node)
{
    uint32_t index = static_cast<uint32_t>(m_States.size());
    StateMachineState state;
    state.Name = std::move(name);
    state.Node = std::move(node);
    m_States.push_back(std::move(state));
    return index;
}

void StateMachineNode::AddTransition(uint32_t fromState, uint32_t toState, float duration,
                                     std::vector<TransitionCondition> conditions)
{
    if (fromState >= m_States.size())
        return;

    StateTransition transition;
    transition.TargetStateIndex = toState;
    transition.Duration = std::max(0.0f, duration);
    transition.Conditions = std::move(conditions);
    m_States[fromState].Transitions.push_back(std::move(transition));
}

void StateMachineNode::SetParam(std::string_view name, ParamValue value)
{
    SetParam(HashStringId(name), std::move(value));
}

void StateMachineNode::SetParam(::GameEngine::StringId id, ParamValue value)
{
    m_Params[id] = std::move(value);
}

const ParamValue* StateMachineNode::GetParam(::GameEngine::StringId id) const
{
    auto it = m_Params.find(id);
    if (it == m_Params.end())
        return nullptr;
    return &it->second;
}

void StateMachineNode::SetActiveState(uint32_t stateIndex)
{
    if (stateIndex >= m_States.size())
        return;
    m_ActiveState = stateIndex;
    m_Transitioning = false;
}

uint32_t StateMachineNode::GetActiveStateIndex() const
{
    return m_ActiveState;
}

const std::vector<StateMachineState>& StateMachineNode::GetStates() const { return m_States; }

bool StateMachineNode::CheckConditions(const std::vector<TransitionCondition>& conditions,
                                       const EvaluationContext& ctx) const
{
    for (const auto& cond : conditions)
    {
        const ::GameEngine::StringId id = cond.ParamId != 0 ? cond.ParamId : HashStringId(cond.ParamName);
        const ParamValue* value = GetParam(id);
        if (!value && ctx.Parameters)
        {
            auto it = ctx.Parameters->find(id);
            if (it != ctx.Parameters->end())
                value = &it->second.Value;
        }
        if (!value || !cond.Matches(*value))
            return false;
    }
    return true;
}

void StateMachineNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_States.empty())
    {
        outPose.Resize(0);
        return;
    }

    const float deltaTime = ctx.DeltaTime;

    // Check for new transitions from the active state (only when not already transitioning)
    if (!m_Transitioning && m_ActiveState < m_States.size())
    {
        for (const auto& transition : m_States[m_ActiveState].Transitions)
        {
            if (transition.TargetStateIndex >= m_States.size())
                continue;
            if (!CheckConditions(transition.Conditions, ctx))
                continue;

            m_Transitioning = true;
            m_TransitionTarget = transition.TargetStateIndex;
            m_TransitionDuration = transition.Duration;
            m_TransitionElapsed = 0.0f;
            break;
        }
    }

    if (m_Transitioning)
    {
        m_TransitionElapsed += deltaTime;

        // Instant transition when duration is zero
        if (m_TransitionDuration <= 0.0f)
        {
            m_ActiveState = m_TransitionTarget;
            m_Transitioning = false;
            if (m_States[m_ActiveState].Node)
                m_States[m_ActiveState].Node->Evaluate(ctx, outPose);
            else
                outPose.Resize(0);
            return;
        }

        float blendWeight = std::clamp(m_TransitionElapsed / m_TransitionDuration, 0.0f, 1.0f);

        AnimationPose poseFrom;
        AnimationPose poseTo;
        if (m_States[m_ActiveState].Node)
            EvaluateAtWeight(*m_States[m_ActiveState].Node, ctx, 1.0f - blendWeight, poseFrom);
        else
            poseFrom.Resize(0);
        if (m_States[m_TransitionTarget].Node)
            EvaluateAtWeight(*m_States[m_TransitionTarget].Node, ctx, blendWeight, poseTo);
        else
            poseTo.Resize(0);

        AnimationPose::Blend(poseFrom, poseTo, blendWeight, outPose);

        // Transition complete
        if (m_TransitionElapsed >= m_TransitionDuration)
        {
            m_ActiveState = m_TransitionTarget;
            m_Transitioning = false;
        }
        return;
    }

    if (m_States[m_ActiveState].Node)
        m_States[m_ActiveState].Node->Evaluate(ctx, outPose);
    else
        outPose.Resize(0);
}

} // namespace Animation
} // namespace GameEngine

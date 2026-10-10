#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimationPose.h"
#include "Types/StringId.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Animation
{

struct BlendSpace1DSample
{
    std::unique_ptr<AnimGraphNode> Node;
    float Position = 0.0f;
};

class BlendSpace1DNode : public AnimGraphNode
{
public:
    void AddSample(std::unique_ptr<AnimGraphNode> node, float position);
    void Sort();

    void SetParameter(float value) { m_Parameter = value; }
    float GetParameter() const { return m_Parameter; }
    void SetParameterName(std::string_view name);
    ::GameEngine::StringId GetParameterId() const { return m_ParameterId; }
    const std::string& GetParameterName() const { return m_ParameterName; }
    const std::vector<BlendSpace1DSample>& GetSamples() const { return m_Samples; }

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::vector<BlendSpace1DSample> m_Samples;
    float m_Parameter = 0.0f;
    std::string m_ParameterName;
    ::GameEngine::StringId m_ParameterId = 0;
    AnimationPose m_TempPose;
};

} // namespace GameEngine::Animation

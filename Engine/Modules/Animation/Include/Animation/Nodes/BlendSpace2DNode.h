#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimationPose.h"
#include "Types/StringId.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Animation
{

struct BlendSpace2DSample
{
    std::unique_ptr<AnimGraphNode> Node;
    float X = 0.0f;
    float Y = 0.0f;
};

class BlendSpace2DNode : public AnimGraphNode
{
public:
    void AddSample(std::unique_ptr<AnimGraphNode> node, float x, float y);

    void SetParameter(float x, float y);
    void SetParameterNameX(std::string_view name);
    void SetParameterNameY(std::string_view name);
    const std::string& GetParameterNameX() const { return m_ParameterNameX; }
    const std::string& GetParameterNameY() const { return m_ParameterNameY; }

    /// Maximum number of nearest samples to blend. Keeps evaluation cost bounded.
    void SetMaxBlendedSamples(uint32_t count) { m_MaxBlendedSamples = count; }
    uint32_t GetMaxBlendedSamples() const { return m_MaxBlendedSamples; }
    const std::vector<BlendSpace2DSample>& GetSamples() const { return m_Samples; }

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::vector<BlendSpace2DSample> m_Samples;
    float m_ParamX = 0.0f;
    float m_ParamY = 0.0f;
    std::string m_ParameterNameX;
    std::string m_ParameterNameY;
    ::GameEngine::StringId m_ParameterIdX = 0;
    ::GameEngine::StringId m_ParameterIdY = 0;
    uint32_t m_MaxBlendedSamples = 4;

    AnimationPose m_TempPoseA;
    AnimationPose m_TempPoseB;
};

} // namespace GameEngine::Animation

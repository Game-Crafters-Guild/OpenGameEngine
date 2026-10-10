#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/PoseUtilities.h"
#include "Mathematics/Vector3.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Animation
{

// Procedural look-at node that rotates a bone to aim toward a target.
// Commonly used for head/eye tracking with configurable angle limits.
class LookAtNode : public AnimGraphNode
{
public:
    void SetSource(std::unique_ptr<AnimGraphNode> node);
    void SetBoneIndex(uint32_t boneIndex);
    void SetTarget(const Mathematics::Vector3& worldTarget);
    void SetWeight(float weight);
    void SetParentIndices(const std::vector<int32_t>& parentIndices);

    // Which local axis of the bone should point toward the target.
    // Default: Z+ forward (engine's left-handed convention).
    void SetAimAxis(const Mathematics::Vector3& axis);

    // Maximum rotation angle from rest pose direction (radians).
    void SetMaxAngle(float maxAngleRadians);

    const AnimGraphNode* GetSource() const;
    uint32_t GetBoneIndex() const;
    float GetWeight() const;
    const Mathematics::Vector3& GetAimAxis() const;
    float GetMaxAngle() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    static constexpr float kDefaultMaxAngle = 1.0472f; // ~60 degrees

    std::unique_ptr<AnimGraphNode> m_Source;
    uint32_t m_BoneIndex = 0;
    Mathematics::Vector3 m_Target{0.0f, 0.0f, 0.0f};
    float m_Weight = 1.0f;
    std::vector<int32_t> m_ParentIndices;
    Mathematics::Vector3 m_AimAxis{0.0f, 0.0f, 1.0f};
    float m_MaxAngle = kDefaultMaxAngle;

    // Persistent scratch reused across Evaluate calls.
    WorldSpacePose m_ScratchWorld;
};

} // namespace GameEngine::Animation

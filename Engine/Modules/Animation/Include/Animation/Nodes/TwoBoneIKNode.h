#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/PoseUtilities.h"
#include "Mathematics/Vector3.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Animation
{

// Analytical two-bone IK solver for arms and legs.
// Given a three-bone chain (root -> mid -> tip), solves joint angles
// so the tip reaches a world-space target position.
class TwoBoneIKNode : public AnimGraphNode
{
public:
    void SetSource(std::unique_ptr<AnimGraphNode> node);

    // Bone chain: root -> mid -> tip (e.g., upper_arm -> forearm -> hand)
    void SetBoneIndices(uint32_t rootBone, uint32_t midBone, uint32_t tipBone);

    // World-space target position the tip bone should reach.
    void SetTarget(const Mathematics::Vector3& target);

    // Pole vector: the mid-joint bends toward this point.
    void SetPoleTarget(const Mathematics::Vector3& poleTarget);

    // Blend weight (0 = no IK, 1 = full IK).
    void SetWeight(float weight);

    // Parent indices from skeleton hierarchy.
    void SetParentIndices(const std::vector<int32_t>& parentIndices);

    const AnimGraphNode* GetSource() const;
    uint32_t GetRootBone() const;
    uint32_t GetMidBone() const;
    uint32_t GetTipBone() const;
    float GetWeight() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

    // Static helper used by composite IK nodes (e.g. FootIKNode). Applies a
    // two-bone IK solve directly to outPose using the provided chain indices,
    // world target, and blend weight. Pole vector defaults to a sensible
    // perpendicular based on the chain's current bend.
    static void SolveTwoBoneIK(
        AnimationPose& outPose,
        const std::vector<int32_t>& parentIndices,
        uint32_t rootBone,
        uint32_t midBone,
        uint32_t tipBone,
        const Mathematics::Vector3& target,
        float weight);

private:
    std::unique_ptr<AnimGraphNode> m_Source;
    uint32_t m_RootBone = 0;
    uint32_t m_MidBone = 0;
    uint32_t m_TipBone = 0;
    Mathematics::Vector3 m_Target{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_PoleTarget{0.0f, 0.0f, 0.0f};
    float m_Weight = 1.0f;
    std::vector<int32_t> m_ParentIndices;

    // Persistent scratch for ComputeWorldSpace; resized once per skeleton.
    WorldSpacePose m_ScratchWorld;
};

} // namespace GameEngine::Animation

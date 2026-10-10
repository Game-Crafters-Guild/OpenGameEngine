#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/PoseUtilities.h"

#include <Mathematics/Vector3.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Animation
{

struct AnimationPose;

struct FootIKLeg
{
    uint32_t HipBone = 0;
    uint32_t KneeBone = 0;
    uint32_t FootBone = 0;
    Mathematics::Vector3 FootOffset{0.0f, 0.0f, 0.0f};
};

// Raycast result provided by the caller each frame. FootIK does not perform raycasts itself,
// keeping the animation system decoupled from physics.
struct FootIKGroundResult
{
    bool Hit = false;
    Mathematics::Vector3 Position{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 Normal{0.0f, 1.0f, 0.0f};
    float Distance = 0.0f;
};

// Adjusts foot positions and hip offset based on ground height.
// Uses raycasts (provided externally) to find ground under each foot,
// then applies TwoBoneIK to place feet on the surface.
class FootIKNode : public AnimGraphNode
{
public:
    void SetSource(std::unique_ptr<AnimGraphNode> node);

    void AddLeg(const FootIKLeg& leg);

    // Must be called each frame before Evaluate with raycast results for each leg.
    void SetGroundResult(uint32_t legIndex, const FootIKGroundResult& result);

    void SetHipBoneIndex(uint32_t hipBone);
    void SetMaxHipOffset(float maxOffset);
    void SetParentIndices(const std::vector<int32_t>& parentIndices);
    void SetWeight(float weight);

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::unique_ptr<AnimGraphNode> m_Source;
    std::vector<FootIKLeg> m_Legs;
    std::vector<FootIKGroundResult> m_GroundResults;
    uint32_t m_HipBone = 0;
    float m_MaxHipOffset = 0.5f;
    float m_Weight = 1.0f;
    std::vector<int32_t> m_ParentIndices;

    // Persistent scratch reused per Evaluate.
    WorldSpacePose m_ScratchWorld;
    std::vector<float> m_ScratchAdjustments;
};

} // namespace GameEngine::Animation

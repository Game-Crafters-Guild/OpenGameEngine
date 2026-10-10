#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/PoseUtilities.h"
#include "Mathematics/Vector3.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Animation
{

// Forward And Backward Reaching Inverse Kinematics solver.
// Iterative solver for chains of arbitrary length (spines, tails, tentacles).
class FABRIKNode : public AnimGraphNode
{
public:
    void SetSource(std::unique_ptr<AnimGraphNode> node);

    // Chain of bone indices from root to tip.
    void SetChain(const std::vector<uint32_t>& boneIndices);

    void SetTarget(const Mathematics::Vector3& target);
    void SetWeight(float weight);
    void SetParentIndices(const std::vector<int32_t>& parentIndices);

    void SetMaxIterations(uint32_t iterations);
    void SetTolerance(float tolerance);

    const AnimGraphNode* GetSource() const;
    const std::vector<uint32_t>& GetChain() const;
    float GetWeight() const;
    uint32_t GetMaxIterations() const;
    float GetTolerance() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    static constexpr uint32_t kDefaultMaxIterations = 10;
    static constexpr float kDefaultTolerance = 0.001f;

    std::unique_ptr<AnimGraphNode> m_Source;
    std::vector<uint32_t> m_Chain;
    Mathematics::Vector3 m_Target{0.0f, 0.0f, 0.0f};
    float m_Weight = 1.0f;
    std::vector<int32_t> m_ParentIndices;
    uint32_t m_MaxIterations = kDefaultMaxIterations;
    float m_Tolerance = kDefaultTolerance;

    // Persistent scratch buffers; reused across Evaluate calls to avoid
    // per-frame heap allocs.
    WorldSpacePose m_ScratchWorld;
    std::vector<Mathematics::Vector3> m_ScratchPositions;
    std::vector<Mathematics::Vector3> m_ScratchOriginalPositions;
    std::vector<float> m_ScratchBoneLengths;
};

} // namespace GameEngine::Animation

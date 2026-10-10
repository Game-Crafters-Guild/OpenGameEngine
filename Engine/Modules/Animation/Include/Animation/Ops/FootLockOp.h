#pragma once

#include "Animation/OpStackNode.h"
#include "Mathematics/Vector3.h"

#include <array>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Animation
{

// FootLockOp — eliminates foot sliding when source-foot planar speed drops
// below `lockThreshold`. Per round-3 audit: GPU-resident with previous-frame
// double-buffered foot positions; the CPU fallback (used for tests + editor
// preview) keeps prev-frame state inside the op instance.
//
// Algorithm (per plan §3 Stage 6.1):
//   1. Compute the world-space foot position from the target's local pose.
//   2. Read the source foot's prev-frame planar (XZ) world position from
//      m_PrevSrcFootPos. If unavailable, seed with current source foot world
//      pos; current frame contributes no lock decision.
//   3. Planar speed = |currentSrc.xz - prevSrc.xz| / dt. (dt comes from
//      EvaluationContext.DeltaTime; falls back to 1/60 when zero.)
//   4. If speed < lockThreshold AND previous target foot anchor valid:
//      retarget the foot to the prev anchor via closed-form 2-bone IK on
//      the (UpperLeg, LowerLeg, Foot) chain. Pelvis stays put.
//   5. Update the prev-anchor and prev-source-foot trackers.
//
// Closed-form 2-bone IK mirrors TwoBoneIKNode: law-of-cosines for upper/lower
// angles, bend direction taken from the post-IK pose's mid-bone projection
// onto the chain plane (degenerate case picks world up x chain).
//
// Params JSON:
//   { "lockThreshold": 0.1, "blend": 1.0 }
// Both optional; defaults match RetargetMap.FootLockSettings.
class FootLockOp final : public OpStackNode
{
public:
    explicit FootLockOp(const nlohmann::json& params);

    void Execute(const OpExecuteContext& ctx, AnimationPose& outPose) override;
    const char* Name() const override { return "FootLock"; }

    static std::unique_ptr<OpStackNode> Create(const nlohmann::json& params);

    float LockThreshold() const { return m_LockThreshold; }
    float Blend() const          { return m_Blend; }

    // Test hooks. Index: 0 = left, 1 = right.
    bool HasPrevAnchor(int footIdx) const { return m_PrevAnchorValid[footIdx]; }
    Mathematics::Vector3 PrevAnchor(int footIdx) const { return m_PrevAnchor[footIdx]; }

    // Reset prev-frame trackers (e.g., on teleport).
    void Reset();

private:
    float m_LockThreshold = 0.1f; // m/s
    float m_Blend         = 1.0f;

    // Per-foot trackers. [0] = left, [1] = right.
    std::array<Mathematics::Vector3, 2> m_PrevSrcFootPos{};
    std::array<bool, 2>                  m_PrevSrcValid{ false, false };
    std::array<Mathematics::Vector3, 2> m_PrevAnchor{};
    std::array<bool, 2>                  m_PrevAnchorValid{ false, false };
};

} // namespace Animation
} // namespace GameEngine

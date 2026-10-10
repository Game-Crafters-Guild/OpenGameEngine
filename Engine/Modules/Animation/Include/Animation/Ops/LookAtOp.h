#pragma once

#include "Animation/OpStackNode.h"
#include "Mathematics/Vector3.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Animation
{

// LookAtOp — drives the Head and (when mapped) LeftEye / RightEye target
// bones to aim at a world-space target point.
//
// Algorithm (per plan §3 Stage 6.1, simplified for v1):
//   1. Compute the head bone's world-space position via parent chain walk
//      from outPose's local rotations.
//   2. aimDir = normalize(target - headWorldPos).
//   3. For Head: build a world rotation that maps the head's "forward" axis
//      (target-skeleton +Z by convention) to aimDir. Convert to local space
//      and write into outPose at the head bone index.
//   4. For Eyes: same, with the eye bone's world position as origin. Eyes
//      spread independently of head.
//
// Phase 5 hardwires the world target via JSON params:
//   { "target": [x, y, z] }
// or
//   { "targetX": x, "targetY": y, "targetZ": z }
// Missing/invalid params disable the op (no aim adjustment). Phase 6 ECS
// integration will overwrite the target each frame from a peer entity.
class LookAtOp final : public OpStackNode
{
public:
    explicit LookAtOp(const nlohmann::json& params);

    void Execute(const OpExecuteContext& ctx, AnimationPose& outPose) override;
    const char* Name() const override { return "LookAt"; }

    static std::unique_ptr<OpStackNode> Create(const nlohmann::json& params);

    // Test hook: read the parsed world target. valid=false when params
    // didn't yield a usable target.
    bool HasTarget() const { return m_HasTarget; }
    Mathematics::Vector3 Target() const { return m_Target; }

private:
    Mathematics::Vector3 m_Target;
    bool                 m_HasTarget = false;
};

} // namespace Animation
} // namespace GameEngine

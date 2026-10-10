#pragma once

#include "Animation/OpStackNode.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Animation
{

// AttachmentPassthroughOp — drives target attachment bones from the source's
// local rotation of the equivalent canonical-bone parent.
//
// For each AttachmentBone in TargetRig.Attachments() with mode == CopyLocal:
//   1. Look up the source bone whose canonical slot matches the
//      AttachmentBone's ParentBone (the canonical anchor declared on the
//      attachment).
//   2. Copy that source bone's local rotation onto the target attachment
//      bone's local rotation in outPose.
//   3. Translation stays at the AttachmentBone.LocalOffset value (already
//      seeded into the target bind pose).
//
// Procedural / Static modes are pass-throughs in v1 (Procedural ops attach
// custom drivers which the user wires via separate components; Static holds
// the pose unchanged).
//
// Params JSON: empty in v1. The param blob is reserved for per-op overrides
// in future phases (e.g. blendAlpha, bone-name overrides).
class AttachmentPassthroughOp final : public OpStackNode
{
public:
    explicit AttachmentPassthroughOp(const nlohmann::json& params);

    void Execute(const OpExecuteContext& ctx, AnimationPose& outPose) override;
    const char* Name() const override { return "AttachmentPassthrough"; }

    // Static factory used by RegisterOpFactory.
    static std::unique_ptr<OpStackNode> Create(const nlohmann::json& params);
};

} // namespace Animation
} // namespace GameEngine

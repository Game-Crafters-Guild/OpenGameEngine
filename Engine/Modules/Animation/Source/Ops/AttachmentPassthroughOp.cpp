#include "Animation/Ops/AttachmentPassthroughOp.h"

#include "Animation/AnimationPose.h"
#include "Animation/HumanoidRig.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"

namespace GameEngine
{
namespace Animation
{

namespace
{

// Resolve the source-skeleton bone index for a canonical HumanBone slot via
// the HumanoidRig.BoneMap. Returns -1 when not mapped or out of range.
int FindSrcBoneByCanonical(const HumanoidRig& rig,
                           const SkeletonData& skel,
                           HumanBone canonical)
{
    if (canonical == HumanBone::None) return -1;
    for (const auto& m : rig.BoneMap())
    {
        if (m.Canonical != canonical) continue;
        if (m.CachedSourceIndex != ~0u && m.CachedSourceIndex < skel.BoneCount)
            return static_cast<int>(m.CachedSourceIndex);
        // Fallback: name lookup. The cached index is only populated after the
        // resolver runs against a SkeletonData; tests call Execute directly.
        for (uint32 b = 0; b < skel.BoneCount; ++b)
        {
            if (b < skel.BoneNames.size() && skel.BoneNames[b] == m.SourceBoneName)
                return static_cast<int>(b);
        }
        return -1;
    }
    return -1;
}

// Locate the target-skeleton bone matching an AttachmentBone.Name. Attachments
// are not part of the canonical 60-bone humanoid; resolve by name only.
int FindTgtBoneByName(const SkeletonData& skel, const std::string& name)
{
    for (uint32 b = 0; b < skel.BoneCount; ++b)
        if (b < skel.BoneNames.size() && skel.BoneNames[b] == name)
            return static_cast<int>(b);
    return -1;
}

} // namespace

AttachmentPassthroughOp::AttachmentPassthroughOp(const nlohmann::json& /*params*/)
{
    // No params consumed in v1.
}

void AttachmentPassthroughOp::Execute(const OpExecuteContext& ctx, AnimationPose& outPose)
{
    if (!ctx.TargetRig || !ctx.SourceRig || !ctx.SourcePose) return;
    if (!ctx.Eval || !ctx.Eval->TargetSkeleton || !ctx.Eval->SourceSkeleton) return;

    const auto& tgtSkel = *ctx.Eval->TargetSkeleton;
    const auto& srcSkel = *ctx.Eval->SourceSkeleton;
    const auto& srcPose = *ctx.SourcePose;

    for (const AttachmentBone& att : ctx.TargetRig->Attachments())
    {
        // Static and Procedural attachments are not driven by passthrough in
        // v1; leave the post-IK pose untouched for them.
        if (att.Mode != AttachmentPassthroughMode::CopyLocal) continue;

        const int tgtIdx = FindTgtBoneByName(tgtSkel, att.Name);
        if (tgtIdx < 0 || static_cast<uint32_t>(tgtIdx) >= outPose.BoneCount) continue;

        const int srcIdx = FindSrcBoneByCanonical(*ctx.SourceRig, srcSkel, att.ParentBone);
        if (srcIdx < 0 || static_cast<uint32_t>(srcIdx) >= srcPose.BoneCount) continue;

        // Copy source local rotation onto target attachment. Translation stays
        // at the rest-pose / LocalOffset value already seeded by Phase 3
        // Stage 3 (TranslationBones force).
        outPose.Rotations[tgtIdx] = srcPose.Rotations[srcIdx];
    }
}

std::unique_ptr<OpStackNode> AttachmentPassthroughOp::Create(const nlohmann::json& params)
{
    return std::make_unique<AttachmentPassthroughOp>(params);
}

} // namespace Animation
} // namespace GameEngine

#pragma once

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Mathematics/Quaternion.h"

#include <vector>

namespace GameEngine
{
namespace Animation
{

class AnimationClip;
struct SkeletonData;

// Editor-side helpers for the three retarget-pose authoring buttons described
// in plan §4 Phase 7b. None of these touch I/O — the inspector calls them on
// a HumanoidRig in memory and then writes the asset back to disk via the
// existing Save path.
//
// Each helper sets per-bone `RetargetPoseRotation` quaternions; none of them
// modify cached source indices, chains, attachments, or the asset's GUID.

// "Force bind = retarget pose" — collapse every retarget delta to identity.
// Used when the source rig was authored in canonical T-pose so no fix-up is
// needed. Touches every bone in the BoneMap.
void ForceRetargetPoseToIdentity(HumanoidRig& rig);

// "Capture from preview" — take a snapshot of an editor-supplied per-bone
// local rotation array, indexed by canonical-bone-row in the rig's BoneMap.
// `localRotations.size()` must equal `rig.BoneMap().size()`. Bones whose
// captured rotation is within `epsilonDeg` of identity collapse to identity
// (the same rule HumanoidImport::BakeRetargetPoseFromAPose uses) so the
// retarget pipeline still treats them as "no fix-up needed".
//
// The inspector populates `localRotations` from the active AnimationPreview
// state; this function is purely the data-side commit.
void CaptureRetargetPoseFromPreview(HumanoidRig& rig,
                                    const std::vector<Mathematics::Quaternion>& localRotations,
                                    float epsilonDeg = 5.0f);

// "Bake from clip frame N" — sample the supplied clip at `timeSeconds`,
// produce per-bone local rotations from the clip's bone tracks (matched by
// `SourceBoneName`), and write them into the rig's RetargetPoseRotation
// slots. Bones not present in the clip retain their existing rotation.
//
// `skel` provides the runtime bone-name -> bone-index map used to resolve
// each rig mapping's `SourceBoneName` to a clip track. Returns the number
// of mappings updated. The inspector builds the time slider against the
// clip's duration and feeds the resulting time value here.
int BakeRetargetPoseFromClipFrame(HumanoidRig& rig,
                                  const AnimationClip& clip,
                                  const SkeletonData& skel,
                                  float timeSeconds,
                                  float epsilonDeg = 5.0f);

} // namespace Animation
} // namespace GameEngine

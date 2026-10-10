#pragma once

#include "Animation/HumanBone.h"
#include "Animation/HumanoidNameMatcher.h"
#include "Animation/HumanoidRig.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

// Forward declaration so HumanoidImport.h doesn't drag cgltf into every
// translation unit that includes it.
struct cgltf_data;

namespace GameEngine
{
namespace Animation
{

// Result of the 3-stage auto-import dispatch. Identifies which path
// produced the rig so callers can log + surface in the editor banner.
enum class HumanoidImportPath : uint8
{
    None = 0,        // No path produced a usable rig.
    VRM1,            // VRMC_vrm extension parsed (or VRM 0.x flipped).
    KHRHumanoid,     // KHR_humanoid glTF extension parsed.
    Heuristic        // Phase 2a regex name-matcher path.
};

const char* HumanoidImportPathToString(HumanoidImportPath path);

// Result of A-pose detection. ShoulderToWristAngleDegLeft and
// ShoulderToWristAngleDegRight are the inferred angle of the upper-arm-to-
// wrist vector from world horizontal (the X axis), measured in degrees, on
// each side. IsAPose is true when EITHER side angles down past 30 deg with a
// plausible arm length. Undetected sides have angle = 0 and presence = false.
struct APoseDetectionResult
{
    bool IsAPose = false;
    bool LeftSidePresent = false;
    bool RightSidePresent = false;
    float ShoulderToWristAngleDegLeft = 0.0f;
    float ShoulderToWristAngleDegRight = 0.0f;
};

// True when the rest pose of the source skeleton, viewed via the canonical
// mapping, has the arms hanging down 30+ degrees from horizontal (Synty-
// style A-pose). T-pose returns false. The detection walks parent-rest-
// translation chains so the source skeleton must have its hierarchy + rest
// translations populated.
APoseDetectionResult DetectAPose(const SkeletonData& skel,
                                 const HumanoidNameMatcher::Result& mapping);

// For each canonically-mapped bone where the source rest rotation differs
// significantly from the profile reference T-pose, computes the per-bone
// rotation that takes bind pose -> canonical T-pose. The values land in
// HumanoidRig.BoneMap[i].RetargetPoseRotation. T-pose-bind rigs (where rest
// matches profile within 5 degrees) get identity quaternions — the math
// pipeline knows identity means "no fix-up needed".
void BakeRetargetPoseFromAPose(const SkeletonData& skel,
                               const SkeletonProfile& profile,
                               HumanoidRig& outRig);

// One-shot rig builder. Runs the matcher on the skeleton bone names,
// computes BodyProportions from world-rest positions, derives canonical
// chains (LeftArm = Shoulder->Hand, etc.), and bakes per-bone retarget pose
// rotations from rest-pose deltas. Returns true when canonical-required-
// bone coverage clears the kCoverageThreshold gate AND rest pose is not
// four-legged (mapped hands at foot height).
bool AutoImportHumanoidRig(const SkeletonData& skel,
                           const SkeletonProfile& profile,
                           HumanoidRig& outRig);

// Threshold for AutoImportHumanoidRig success. >=80 percent of required
// canonical bones must map for the import to be considered usable.
constexpr float kAutoImportCoverageThreshold = 0.8f;

// 3-stage dispatch — VRM 1.0 -> KHR_humanoid -> heuristic name matcher.
// First path that succeeds wins; the path taken is reported via outPath
// (for the editor banner / log line).
//
// `gltf` may be null when the source isn't glTF (e.g., FBX): in that case
// the dispatch falls straight through to the heuristic path. When the
// source is glTF, both KHR_humanoid + VRM 1.0 are probed first; if neither
// is present (or either's payload fails the coverage gate), the heuristic
// path runs as the fallback.
//
// Logs at INFO which path produced the rig so the editor / batch importer
// can surface it. Returns true when *any* path succeeds.
bool AutoImportHumanoidRigDispatch(const cgltf_data* gltf,
                                   const SkeletonData& skel,
                                   const SkeletonProfile& profile,
                                   HumanoidRig& outRig,
                                   HumanoidImportPath& outPath);

// Builds a default RetargetMap pairing src -> tgt. Sets SourceRigRef +
// TargetRigRef GUIDs, populates ChainPairing entries for every canonical
// chain present on BOTH rigs (defaults: FK rotation OneToOne alpha 1.0,
// translation None, IK off). Op stack is left empty.
void AutoCreateRetargetMap(const HumanoidRig& src,
                           const HumanoidRig& tgt,
                           RetargetMap& outMap);

} // namespace Animation
} // namespace GameEngine

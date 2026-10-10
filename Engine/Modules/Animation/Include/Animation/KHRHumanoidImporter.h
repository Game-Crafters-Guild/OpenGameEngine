#pragma once

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include <string>
#include <string_view>

// Forward declaration so callers can include this header without dragging in
// cgltf. The implementation in KHRHumanoidImporter.cpp does the actual
// `#include <cgltf.h>`.
struct cgltf_data;

namespace GameEngine
{
namespace Animation
{

// glTF KHR_humanoid extension importer.
//
// `KHR_humanoid` is a draft Khronos extension (mirrors the VRM 1.0 humanoid
// schema) that pre-declares per-canonical-bone -> glTF-node bindings on a
// glTF document. When this extension is present we skip the regex-name
// heuristic from `HumanoidNameMatcher` and use the explicit mapping
// directly — eliminating any false-match risk on rigs with unusual bone
// names.
//
// Schema (informal, mirrors VRM 1.0):
// {
//   "extensions": {
//     "KHR_humanoid": {
//       "humanBones": {
//         "hips":           { "node": <int> },
//         "spine":          { "node": <int> },
//         ...                                      // 24 body bones + 30 fingers
//         "leftThumbProximal": { "node": <int> }
//       }
//     }
//   }
// }
//
// Per-bone presence is optional; missing bones leave the mapping unmapped
// (CachedSourceIndex = ~0u). The caller still applies the same coverage
// gate as the heuristic path. The bone-name strings in the extension are
// camelCase (matches the VRM 1.0 spec); we normalize on parse.

// Returns true when the glTF document carries a `KHR_humanoid` extension
// and the bone-binding map populates a HumanoidRig that clears the
// kAutoImportCoverageThreshold gate. Returns false when:
//   * `gltf` is null,
//   * the extension is absent,
//   * the JSON payload is malformed,
//   * fewer than the threshold of *required* canonical bones are bound.
//
// On success `outRig` is overwritten: BoneMap, Chains, BodyProportions,
// TranslationBones, retarget pose deltas, and ProfileRef are all populated
// (the retarget-pose bake reuses the same `BakeRetargetPoseFromAPose`
// helper as the heuristic path so an A-pose KHR rig still bakes a fix-up
// rotation from rest deltas).
bool ImportFromKHRHumanoid(const cgltf_data* gltf,
                           const SkeletonData& skel,
                           const SkeletonProfile& profile,
                           HumanoidRig& outRig);

// Returns true when a `KHR_humanoid` extension entry exists at the glTF
// document root. Cheap probe used by the auto-import dispatcher to
// pick the import path without committing to a full parse.
bool HasKHRHumanoidExtension(const cgltf_data* gltf);

// Map a VRM 1.0 / KHR_humanoid bone-name string (camelCase, e.g.
// "leftUpperArm") to the canonical HumanBone enum. Unknown names resolve
// to HumanBone::None. Used internally + by tests to assert the bone-name
// table is complete.
HumanBone HumanBoneFromKHRName(std::string_view khrName);

} // namespace Animation
} // namespace GameEngine

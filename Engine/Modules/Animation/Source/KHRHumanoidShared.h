#pragma once

// Shared helpers between KHRHumanoidImporter.cpp and VRM1Importer.cpp.
// Both extensions use the same `humanBones` JSON shape; this header keeps
// the canonical parse path private to the Animation module's importers.

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include <nlohmann/json.hpp>

struct cgltf_data;
struct cgltf_extension;
struct cgltf_node;

namespace GameEngine
{
namespace Animation
{
namespace KHRDetail
{

// Parse a `humanBones` JSON node (the same shape KHR_humanoid + VRMC_vrm
// share). Populates outRig and runs the retarget-pose bake. Returns false
// when required-bone coverage falls below kAutoImportCoverageThreshold.
//
// `gltf` may be null only when the SkeletonData has populated BoneNames
// matching the source-bone names referenced by node lookup; for the
// runtime path it is always non-null.
bool ImportFromHumanBonesJson(const nlohmann::json& humanBones,
                              const cgltf_data* gltf,
                              const SkeletonData& skel,
                              const SkeletonProfile& profile,
                              HumanoidRig& outRig);

// Find a named extension entry on the document root. Returns nullptr when
// absent. Cheap O(N) scan; N <= a handful in practice.
const cgltf_extension* FindRootExtension(const cgltf_data* gltf,
                                         const char* name);

// Map cgltf_node* to global node index; returns -1 if absent.
int32 NodeIndex(const cgltf_data* gltf, const ::cgltf_node* node);

// Map a node index (in cgltf->nodes) to a SkeletonData skin-joint index, or
// fall back to bone-name lookup. Returns ~0u on miss.
uint32 ResolveSkinJoint(const cgltf_data* gltf, const SkeletonData& skel,
                        uint32 nodeIdx, std::string& outSourceName);

} // namespace KHRDetail
} // namespace Animation
} // namespace GameEngine

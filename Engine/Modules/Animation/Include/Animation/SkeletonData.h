#pragma once

#include "Types/Types.h"
#include "Types/StringId.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine { namespace Animation {

// Per-skeleton authored data. Bone hierarchy + rest pose + skin metadata.
//
// Lives in the Animation module so that retargeting / IK / pose evaluation
// can consume it without depending on the rendering pipeline. The legacy
// `Engine::Renderer::SkeletonData` symbol still resolves via a `using`
// alias in `Engine/ECSModules/Rendering/Include/ECSModules/Rendering/SkeletonStore.h`.
struct SkeletonData {
    // Hierarchy
    std::vector<int32> Parent;          // size = BoneCount

    // Rest/default local transforms per bone (used when no animation channel overrides)
    // Stored both as TRS components and as the original 4x4 local matrix (column-major) to preserve exact bind pose.
    std::vector<float> RestTranslation; // size = BoneCount * 3
    std::vector<float> RestRotation;    // size = BoneCount * 4 (x,y,z,w)
    std::vector<float> RestScale;       // size = BoneCount * 3
    std::vector<float> RestLocalMatrix; // size = BoneCount * 16 (column-major 4x4)

    // Bind/inverse bind (column-major 4x4 per bone)
    std::vector<float> BindPose;        // size = BoneCount * 16
    std::vector<float> InverseBind;     // size = BoneCount * 16

    // World-space anchors captured at load (column-major 4x4)
    // SkeletonRootWorld: world transform of the skeleton root node (cgltf_skin::skeleton), or identity if unknown
    // MeshRootWorld: world transform of the mesh node using this skin; used to produce model-relative bones
    float SkeletonRootWorld[16] { 1,0,0,0,
                                  0,1,0,0,
                                  0,0,1,0,
                                  0,0,0,1 };
    float MeshRootWorld[16]      { 1,0,0,0,
                                  0,1,0,0,
                                  0,0,1,0,
                                  0,0,0,1 };

    uint32 BoneCount = 0;

    // Bone names per index. Kept for inspector / log diagnostics; the
    // sample-time hot path uses BoneNameIds + BoneNameLookup instead.
    std::vector<std::string> BoneNames; // size = BoneCount when populated

    // Hashed bone names (FNV-1a 64-bit StringId). Sampler uses this for
    // cross-rig channel resolution: a clip's `AnimChannel.targetNameId`
    // is looked up in `BoneNameLookup` to find the destination bone index
    // regardless of export-time index reordering or extra helper nodes
    // between rigs. Index 0 is the "no name" sentinel.
    std::vector<StringId> BoneNameIds; // size = BoneCount when populated

    // Built once after BoneNameIds is populated. Read-only afterwards.
    // Skips entries with id == 0 so empty-name lookups always miss
    // rather than colliding into a nameless slot.
    std::unordered_map<StringId, uint32> BoneNameLookup;

    // Hash BoneNames into BoneNameIds and rebuild BoneNameLookup. Loaders
    // call this once after BoneNames + BoneCount are filled. Idempotent.
    void BuildBoneNameLookup();

    // Resolve a clip-channel target by hashed bone name to a destination
    // bone index. Returns `fallbackIndex` if the id is the null sentinel
    // (0), the lookup is empty, or no match exists. Hot-path contract:
    // one hashmap probe, no allocation, no string compare.
    uint32 ResolveBoneIndex(StringId nameId, uint32 fallbackIndex) const;

    // Spec-accurate (skin-order) metadata
    // JointNodes[j] = node index of skin.joints[j]
    uint32 SkinJointCount = 0;
    std::vector<uint32> JointNodes;             // length = SkinJointCount

    // Optional: node index of the mesh that uses this skeleton (for model-relative skinning)
    int32 MeshRootNode = -1; // -1 if unknown

    // Bones sorted parent-before-child, level by level from the roots.
    // Computed once by ComputeTopologicalSort() after hierarchy is populated.
    std::vector<uint32> TopologicalOrder;  // size = BoneCount

    // The joint closure: every skin joint and every ancestor up to its root,
    // the bones the GPU animation path evaluates (a bone outside it never
    // reaches the skin palette). Sorted like TopologicalOrder; level L holds
    // JointClosure[JointClosureLevelOffsets[L] .. JointClosureLevelOffsets[L+1]).
    // Without a joint table (SkinJointCount 0, or JointNodes not matching it)
    // every bone skins as a joint, so the closure is every bone.
    std::vector<uint32> JointClosure;             // bone index per closure bone
    std::vector<uint32> JointClosureLevelOffsets; // size = JointClosureLevelCount() + 1

    uint32 JointClosureLevelCount() const;

    // Compute TopologicalOrder and the joint closure from Parent and the joint
    // table via BFS. Call after Parent, SkinJointCount and JointNodes are
    // populated. Idempotent.
    void ComputeTopologicalSort();

    // Source FBX/glTF this skeleton was extracted from. Stored absolute when
    // the asset path is known; populated by the model loader and consumed by
    // the humanoid auto-bootstrap path to locate the matching .humanoidrig.json
    // sidecar. Empty when the skeleton was synthesized at runtime (tests,
    // procedural content).
    std::filesystem::path SourceModelPath;
};

}} // namespace GameEngine::Animation

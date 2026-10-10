#pragma once

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include <string>
#include <vector>

struct cgltf_data;

namespace GameEngine
{
namespace Animation
{

// VRM 1.0 metadata captured during import. Phase 10 imports the bone map
// (the only piece the retarget pipeline needs); the other extension
// surfaces are captured here as data-flow placeholders for downstream
// consumers (face/expression system, look-at runtime, first-person culling).
//
// `expressions` is intentionally a bag-of-strings (clip names); the actual
// blendshape clip data is parsed by the face/expression module.
struct VRM1Metadata
{
    std::string SpecVersion;             // "1.0" / "0.x" (auto-flipped on 0.x).
    bool        IsLegacyVRM0 = false;    // true when the source was VRM 0.x.

    // First-person mesh-annotation passthrough. Each entry pairs a glTF
    // mesh index with one of the canonical FirstPersonType values
    // ("auto"/"both"/"thirdPersonOnly"/"firstPersonOnly").
    struct FirstPersonAnnotation
    {
        int32       MeshIndex = -1;
        std::string Type;
    };
    std::vector<FirstPersonAnnotation> FirstPersonMeshAnnotations;

    // Look-at runtime parameters. Eye offsets are in the head local frame.
    // Range/offsets default to zero when the source omits them.
    struct LookAt
    {
        float OffsetFromHeadX = 0.0f;
        float OffsetFromHeadY = 0.0f;
        float OffsetFromHeadZ = 0.0f;
        std::string Type;                // "bone" / "expression"
    };
    LookAt LookAtSettings;
    bool   HasLookAt = false;

    // Expression clip names (ImportableButIgnored in v1 — face system
    // owns the actual blendshape parse).
    std::vector<std::string> Expressions;

    // Spring bone names (ImportableButIgnored in v1).
    std::vector<std::string> SpringBones;
};

// VRM 1.0 (`VRMC_vrm`) importer. Parses the humanoid bone map and the
// metadata listed in VRM1Metadata. Sets the same HumanoidRig fields as
// the heuristic + KHR paths.
//
// VRM 0.x fallback: when the document carries the old `VRM` extension
// (not `VRMC_vrm`), this function detects it, logs a warning about the
// forward-axis sign flip (VRM 0.x = -Z forward; VRM 1.0 = +Z forward),
// flips the source skeleton's RestRotations on the Y axis to convert
// (the SkeletonData passed in is *not* modified — the flip is applied
// to a working copy used for retarget-pose bake), and produces a
// `HumanoidRig` consistent with the engine's +Z-forward convention.
//
// Returns false when:
//   * `gltf` is null,
//   * neither `VRMC_vrm` nor `VRM` extension is present,
//   * humanoid bone coverage falls below the threshold.
bool ImportFromVRM1(const cgltf_data* gltf,
                    const SkeletonData& skel,
                    const SkeletonProfile& profile,
                    HumanoidRig& outRig,
                    VRM1Metadata& outMeta);

// Cheap probe: returns true when either `VRMC_vrm` or the legacy `VRM`
// extension is present at the document root. Used by the dispatcher.
bool HasVRMExtension(const cgltf_data* gltf);

// Internal helper exposed for tests: applies the VRM 0.x -> 1.0 axis flip
// (180-degree rotation about Y on every bone's local rest rotation). The
// input skeleton is copied; the output is a flipped working copy. Tests
// call this to verify the conversion is bit-stable + idempotent.
SkeletonData ApplyVRM0AxisFlip(const SkeletonData& src);

} // namespace Animation
} // namespace GameEngine

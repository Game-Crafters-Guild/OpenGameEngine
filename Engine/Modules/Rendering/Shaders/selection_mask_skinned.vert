#version 450

// Selection mask draw (skinned meshes).
//
// Samples the shared bone palette atlas SSBO (owned by RenderServices /
// SkinPaletteAtlas, bound at set 0 / binding 12) to produce the animated
// silhouette so the outline follows the posed mesh instead of bind pose.
//
// Bindings match the color/depth skinned variants:
//   - Binding 0, location 0: vec3 position from core interleaved VB
//   - Binding 4, location 6: uvec4 joints (R16G16B16A16_UINT)
//   - Binding 5, location 7: vec4 weights
//
// The push-constant layout is shared with selection_mask.vert. uVPM is
// viewProj * model combined on the CPU. uMaskParams.w stores the atlas
// offset (in bone-slot units; one slot = 3 vec4 rows in the mat3x4 layout,
// see Includes/bone_palette.glsl) for this entity's palette; entities with
// no active animation use offset 0 which SkinPaletteAtlas::BeginFrame fills
// with an identity-matrix block for correct bind-pose rendering.

layout(location = 0) in vec3 aPos;
layout(location = 6) in uvec4 aJoints;
layout(location = 7) in vec4 aWeights;

layout(set = 0, binding = 12) readonly buffer BonePaletteAtlasSSBO
{
    vec4 rows[];
} BonePaletteAtlas;

#define GE_BONE_PALETTE_READABLE
#include "Includes/bone_palette.glsl"

layout(push_constant) uniform PC
{
    mat4 uVPM;
    vec4 uMaskParams;   // x alpha cutoff, yz UV scale, w skin palette offset
    vec4 uMaskExtra;    // xy UV offset, z time seconds, w instance seed
    vec4 uWindStrength; // xyz local wind, w enabled
    vec4 uWindParams;   // x frequency, y spatial scale, z height, w base Y
} pc;

void main()
{
    uint skinPaletteOffset = uint(pc.uMaskParams.w + 0.5);
    vec3 skinnedPos = ge_ApplyBonePalettePosition(aPos, aJoints, aWeights,
                                                  skinPaletteOffset);
    gl_Position = pc.uVPM * vec4(skinnedPos, 1.0);
}
